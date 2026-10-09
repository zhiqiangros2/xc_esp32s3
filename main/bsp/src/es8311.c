/**
 * @file es8311.c
 * @brief BOX3 板载 ES8311 音频编解码器的播放侧驱动。
 *
 * ES8311 通过 I2C0 配置、通过 I2S0 接收 PCM。当前驱动只启用播放所需的
 * DAC 信号路径，不实现麦克风录音接口。调用 es8311_init() 前必须先启动
 * I2S 时钟，否则 Codec 复位和时钟管理寄存器可能无法进入稳定状态。
 */
#include "es8311.h"

#include <stddef.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c.h"

/* 本板 CE 地址选择脚固定为低电平，因此 7 bit I2C 地址固定为 0x18。 */
#define ES8311_I2C_ADDRESS 0x18
/* 使用已验证稳定的 100 kHz；当前板上 400 kHz 传输曾返回 INVALID_STATE。 */
#define ES8311_I2C_FREQUENCY_HZ 100000
/* 每次 I2C 事务最多等待 100 ms，防止总线异常时永久阻塞播放任务。 */
#define ES8311_I2C_TIMEOUT_MS 100
/* 读写失败各自最多尝试 3 次，处理上电初期或共享总线上的瞬时错误。 */
#define ES8311_I2C_READ_MAX_ATTEMPTS 3
#define ES8311_I2C_WRITE_MAX_ATTEMPTS 3
/* I2C 重试及初始化表相邻寄存器写入之间的统一稳定时间，单位毫秒。 */
#define ES8311_I2C_DELAY_MS 10
/* 上电初始化完成后设置 DAC 默认输出音量为 80%。 */
#define ES8311_DEFAULT_OUTPUT_VOLUME_PERCENT 80U

/* ES8311 寄存器地址。按功能分为复位、时钟、串行口、系统、ADC、DAC 和 GPIO。 */
#define ES8311_REG_RESET 0x00
#define ES8311_REG_CLOCK_MANAGER_01 0x01
#define ES8311_REG_CLOCK_MANAGER_02 0x02
#define ES8311_REG_CLOCK_MANAGER_03 0x03
#define ES8311_REG_CLOCK_MANAGER_04 0x04
#define ES8311_REG_CLOCK_MANAGER_05 0x05
#define ES8311_REG_CLOCK_MANAGER_06 0x06
#define ES8311_REG_CLOCK_MANAGER_07 0x07
#define ES8311_REG_CLOCK_MANAGER_08 0x08
#define ES8311_REG_SDP_INPUT 0x09
#define ES8311_REG_SDP_OUTPUT 0x0A
#define ES8311_REG_SYSTEM_0B 0x0B
#define ES8311_REG_SYSTEM_0C 0x0C
#define ES8311_REG_SYSTEM_0D 0x0D
#define ES8311_REG_SYSTEM_0E 0x0E
#define ES8311_REG_SYSTEM_10 0x10
#define ES8311_REG_SYSTEM_11 0x11
#define ES8311_REG_SYSTEM_12 0x12
#define ES8311_REG_SYSTEM_13 0x13
#define ES8311_REG_SYSTEM_14 0x14
#define ES8311_REG_ADC_15 0x15
#define ES8311_REG_ADC_16 0x16
#define ES8311_REG_ADC_17 0x17
#define ES8311_REG_ADC_1B 0x1B
#define ES8311_REG_ADC_1C 0x1C
#define ES8311_REG_DAC_31 0x31
#define ES8311_REG_DAC_32 0x32
#define ES8311_REG_DAC_37 0x37
#define ES8311_REG_GPIO_44 0x44
#define ES8311_REG_GPIO_45 0x45

typedef struct {
    /* 要写入的 8 bit 寄存器地址。 */
    uint8_t address;
    /* 写入该寄存器的完整 8 bit 值。 */
    uint8_t value;
} es8311_register_value_t;

/* I2C 设备句柄在 add_device 成功后有效，失败清理时恢复为 NULL。 */
static board_i2c_device_handle_t s_device;
/* 防止重复探测、重复加入同一个 I2C 设备并重复执行复位序列。 */
static bool s_initialized;
static const char *TAG = "ES8311";

/**
 * @brief 写一个 ES8311 寄存器，并对瞬时 I2C 错误进行有限次数重试。
 *
 * ES8311 的写事务为两个字节：[寄存器地址, 寄存器值]。最终失败时保留底层
 * esp_err_t，便于上层日志判断是超时、NACK 还是总线状态错误。
 */
static esp_err_t write_register(uint8_t address, uint8_t value)
{
    const uint8_t data[] = {address, value};
    esp_err_t result = ESP_FAIL;

    for (unsigned int attempt = 1;
         attempt <= ES8311_I2C_WRITE_MAX_ATTEMPTS;
         ++attempt) {
        result = board_i2c_transmit(s_device,
                                    data,
                                    sizeof(data),
                                    ES8311_I2C_TIMEOUT_MS);
        if (result == ESP_OK) {
            return ESP_OK;
        }

        if (attempt < ES8311_I2C_WRITE_MAX_ATTEMPTS) {
            ESP_LOGW(TAG,
                     "Write register 0x%02X failed: %s; retry %u/%u",
                     address,
                     esp_err_to_name(result),
                     attempt + 1,
                     ES8311_I2C_WRITE_MAX_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(ES8311_I2C_DELAY_MS));
        }
    }

    ESP_LOGE(TAG,
             "Write register 0x%02X failed after %u attempts: %s",
             address,
             ES8311_I2C_WRITE_MAX_ATTEMPTS,
             esp_err_to_name(result));
    return result;
}

/**
 * @brief 读取一个 ES8311 寄存器，并对瞬时 I2C 错误进行有限次数重试。
 *
 * 使用“先发送寄存器地址、再重复起始读取 1 字节”的组合事务，避免发送地址
 * 与读取数据之间释放总线。
 */
static esp_err_t read_register(uint8_t address, uint8_t *value)
{
    esp_err_t result = ESP_FAIL;

    for (unsigned int attempt = 1;
         attempt <= ES8311_I2C_READ_MAX_ATTEMPTS;
         ++attempt) {
        result = board_i2c_transmit_receive(
            s_device,
            &address,
            sizeof(address),
            value,
            sizeof(*value),
            ES8311_I2C_TIMEOUT_MS);
        if (result == ESP_OK) {
            return ESP_OK;
        }

        if (attempt < ES8311_I2C_READ_MAX_ATTEMPTS) {
            ESP_LOGW(TAG,
                     "Read register 0x%02X failed: %s; retry %u/%u",
                     address,
                     esp_err_to_name(result),
                     attempt + 1,
                     ES8311_I2C_READ_MAX_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(ES8311_I2C_DELAY_MS));
        }
    }

    ESP_LOGE(TAG,
             "Read register 0x%02X failed after %u attempts: %s",
             address,
             ES8311_I2C_READ_MAX_ATTEMPTS,
             esp_err_to_name(result));
    return result;
}

/**
 * @brief 对寄存器执行读-改-写，只修改指定的位。
 *
 * 新值计算方式为 (旧值 & ~clear_mask) | set_mask。调用方应让 set_mask 只
 * 包含 clear_mask 范围内的位，以免意外改变其他功能位。
 */
static esp_err_t update_register(uint8_t address,
                                 uint8_t clear_mask,
                                 uint8_t set_mask)
{
    uint8_t value = 0;
    esp_err_t result = read_register(address, &value);
    if (result != ESP_OK) {
        return result;
    }

    value = (uint8_t)((value & (uint8_t)~clear_mask) | set_mask);
    return write_register(address, value);
}

/**
 * @brief 探测并初始化固定地址为 0x18 的 ES8311。
 *
 * 初始化分两阶段：第一阶段建立基础时钟并触发复位，等待内部电路稳定后解除
 * 复位；第二阶段配置 I2S 从机格式、DAC/ADC 信号路径和 GPIO。任何一步失败
 * 都删除 I2C 设备句柄，使后续调用能够完整重试。
 */
esp_err_t es8311_init(void)
{
    /* 已完成初始化时直接返回，保证接口幂等。 */
    if (s_initialized) {
        return ESP_OK;
    }

    /* 先探测固定地址，错误日志能明确区分“器件不存在”和“配置失败”。 */
    esp_err_t result = board_i2c_probe(ES8311_I2C_ADDRESS,
                                       ES8311_I2C_TIMEOUT_MS);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "No ES8311 response at fixed I2C address 0x%02X: %s",
                 ES8311_I2C_ADDRESS,
                 esp_err_to_name(result));
        return result;
    }

    /* 探测成功后才把器件加入共享 I2C 总线。 */
    result = board_i2c_add_device(ES8311_I2C_ADDRESS,
                                  ES8311_I2C_FREQUENCY_HZ,
                                  &s_device);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "Add I2C device 0x%02X failed: %s",
                 ES8311_I2C_ADDRESS,
                 esp_err_to_name(result));
        return result;
    }

    /*
     * 第一阶段配置基础时钟和模拟电源，再通过 RESET 寄存器触发内部复位。
     * 16 位立体声时 BCLK 为 32fs，当前寄存器组合让 ES8311 使用该时钟关系。
     * 数组顺序属于上电时序的一部分，不能按寄存器地址随意排序。
     */
    static const es8311_register_value_t initialization[] = {
        {ES8311_REG_GPIO_45, 0x00},
        {ES8311_REG_CLOCK_MANAGER_01, 0x30},
        {ES8311_REG_CLOCK_MANAGER_02, 0x00},
        {ES8311_REG_CLOCK_MANAGER_03, 0x10},
        {ES8311_REG_ADC_16, 0x24},
        {ES8311_REG_CLOCK_MANAGER_04, 0x10},
        {ES8311_REG_CLOCK_MANAGER_05, 0x00},
        {ES8311_REG_SYSTEM_0B, 0x00},
        {ES8311_REG_SYSTEM_0C, 0x00},
        {ES8311_REG_SYSTEM_10, 0x1F},
        {ES8311_REG_SYSTEM_11, 0x7F},
        {ES8311_REG_RESET, 0x80},
    };

    /* 每次写前保留稳定时间；失败立即转到统一资源清理路径。 */
    for (size_t i = 0; i < sizeof(initialization) / sizeof(initialization[0]); ++i) {
        vTaskDelay(pdMS_TO_TICKS(ES8311_I2C_DELAY_MS));
        result = write_register(initialization[i].address,
                                initialization[i].value);
        if (result != ESP_OK) {
            goto init_failed;
        }
    }
    /* 复位写入后等待 Codec 内部时钟和模拟模块稳定。 */
    vTaskDelay(pdMS_TO_TICKS(80));

    /* 清除 RESET 寄存器的 0x40 位，同时保留其他位当前状态。 */
    result = update_register(ES8311_REG_RESET, 0x40, 0x00);
    if (result != ESP_OK) {
        goto init_failed;
    }

    /*
     * 第二阶段配置时钟分频、I2S 数据格式以及 DAC/模拟输出信号路径。
     * 表内也保留 ADC 相关基础配置，以维持参考初始化序列的完整状态。
     */
    static const es8311_register_value_t clock_and_signal_path[] = {
        {ES8311_REG_SYSTEM_0D, 0x01},
        {ES8311_REG_CLOCK_MANAGER_01, 0xBF},
        {ES8311_REG_CLOCK_MANAGER_02, 0x18},
        {ES8311_REG_CLOCK_MANAGER_05, 0x00},
        {ES8311_REG_CLOCK_MANAGER_03, 0x10},
        {ES8311_REG_CLOCK_MANAGER_04, 0x10},
        {ES8311_REG_CLOCK_MANAGER_07, 0x00},
        {ES8311_REG_CLOCK_MANAGER_08, 0xFF},
        {ES8311_REG_CLOCK_MANAGER_06, 0x03},
        {ES8311_REG_SDP_INPUT, 0x0C},
        {ES8311_REG_SDP_OUTPUT, 0x0C},
        {ES8311_REG_SYSTEM_14, 0x1A},
        {ES8311_REG_SYSTEM_12, 0x00},
        {ES8311_REG_SYSTEM_13, 0x10},
        {ES8311_REG_SYSTEM_0E, 0x02},
        {ES8311_REG_ADC_15, 0x40},
        {ES8311_REG_ADC_1B, 0x0A},
        {ES8311_REG_ADC_1C, 0x6A},
        {ES8311_REG_DAC_37, 0x48},
        {ES8311_REG_GPIO_44, 0x08},
        {ES8311_REG_ADC_17, 0xBF},
        {ES8311_REG_DAC_32, 0xBF},
    };

    for (size_t i = 0;
         i < sizeof(clock_and_signal_path) / sizeof(clock_and_signal_path[0]);
         ++i) {
        vTaskDelay(pdMS_TO_TICKS(ES8311_I2C_DELAY_MS));
        result = write_register(clock_and_signal_path[i].address,
                                clock_and_signal_path[i].value);
        if (result != ESP_OK) {
            goto init_failed;
        }
    }

    /* 公共音量/静音接口要求 initialized=true，因此先置位再调用。 */
    s_initialized = true;
    result = es8311_set_volume(ES8311_DEFAULT_OUTPUT_VOLUME_PERCENT);
    if (result == ESP_OK) {
        /*
         * 初始化完成后解除数字静音。播放器停止时不会重新设置静音，也不会
         * 关闭 I2S、Codec 或板级功放，因此运行期间始终保持音频输出开启。
         */
        result = es8311_set_mute(false);
    }
    if (result == ESP_OK) {
        ESP_LOGI(TAG,
                 "Codec ready: 16-bit stereo I2S slave, volume=%u%%",
                 ES8311_DEFAULT_OUTPUT_VOLUME_PERCENT);
        return ESP_OK;
    }

init_failed:
    /* 包括音量或解除静音失败在内，统一移除设备并恢复未初始化状态。 */
    board_i2c_remove_device(s_device);
    s_device = NULL;
    s_initialized = false;
    return result;
}

/**
 * @brief 设置 DAC 数字音量。
 *
 * 对外使用容易理解的 0~100 百分比，再按四舍五入映射到寄存器 0x32 的
 * 0x00~0xFF 范围。100% 对应 0xFF，当前上电默认值为 80%。
 */
esp_err_t es8311_set_volume(uint8_t percent)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 加 50 后再除以 100，实现整数运算下的四舍五入。 */
    const uint8_t register_value =
        (uint8_t)(((uint16_t)percent * 255U + 50U) / 100U);
    return write_register(ES8311_REG_DAC_32, register_value);
}

/**
 * @brief 设置或解除 DAC 数字静音。
 *
 * 仅更新 DAC_REG31 的 0x60 位域，保留寄存器中与其他 DAC 功能相关的位。
 */
esp_err_t es8311_set_mute(bool muted)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    return update_register(ES8311_REG_DAC_31,
                           0x60,
                           muted ? 0x60 : 0x00);
}
