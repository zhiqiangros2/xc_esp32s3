/**
 * @file es8311.c
 * @brief BOX3 板载 ES8311 音频编解码器的播放侧驱动。
 *
 * ES8311 通过 I2C0 接收寄存器配置，通过 I2S0 的独立 TX 数据线接收播放
 * PCM。ESP32-S3 是 I2S 主机，ES8311 是 I2S 从机；本驱动只对外提供播放
 * 所需的 DAC 控制，初始化表中保留的 ADC 配置仅用于保持正点原子参考时序
 * 完整，ES8311 的数字麦克风输入保持关闭。
 *
 * 从 ES8311 一侧观察，数字音频引脚关系如下：
 *
 *   ESP32 GPIO21 MCLK  --------------------------> MCLK      时钟输入
 *   ESP32 GPIO38 BCLK  --------------------------> SCLK      时钟输入
 *   ESP32 GPIO39 LRCK  --------------------------> LRCK      帧同步输入
 *   ESP32 GPIO40 DOUT  --[L,0,R,0]--------------> DSDIN     播放输入
 *   ESP32 GPIO41 DIN   <--- ES7210，与 ES8311 无数字数据连接
 *
 *   OUTP/OUTN --模拟差分信号--+------------------> 外部功放
 *                             +------------------> ES7210 MIC3（AEC 参考）
 *
 * ES8311 在本项目中只接收播放 PCM；它的 ASDOUT/ADC 串行输出不接 ESP32 的
 * GPIO41。GPIO41 上的数据来自 ES7210，因此不能把 I2S0 RX 理解成 ES8311 ADC。
 * MCLK/BCLK/LRCK 虽与 ES7210 共用，但 DSDIN 仍是一根独立的单向播放线。
 *
 * ES8311 的串行音频口支持 I2S、左对齐和 DSP/PCM A/B；输入字长可配置为
 * 16/18/20/24/32 bit。数据手册总述还提到右对齐，但寄存器 0x09 的格式位
 * 没有独立的右对齐编码，因此本工程不使用也不声明已经验证右对齐。
 *
 * 必须区分“ESP32 物理总线采用四槽帧”和“ES8311 的器件能力”：DSP/PCM
 * 是串行帧格式，不代表 ES8311 是四通道 TDM Codec。ES8311 只有一个单声道
 * DAC，寄存器只能选择左或右串行声道送入该 DAC；OUTP/OUTN 是同一 DAC 的
 * 差分正、负输出，不是左、右声道。该差分信号一方面送往功放，另一方面接到
 * ES7210 MIC3，作为后续 AEC 的播放参考。
 *
 * 本项目的 I2S0 因 ES7210 四路采集需要而统一使用 16 bit、四槽、64fs 帧。
 * 这只是 ESP32 I2S 控制器的传输布局，ES8311 仍按普通 Philips I2S 解析：
 * LRCK 低电平是 32 BCLK 左窗口，LRCK 高电平是 32 BCLK 右窗口；REG09 将
 * 有效字长设为 16 bit，所以 ES8311 每个窗口只接收开头的一个 16 bit 样本，
 * 窗口剩余 16 BCLK 必须作为填充时间。因此 ESP32 的 TX 排列为：
 *
 *   slot0=L，slot1=0，slot2=R，slot3=0
 *
 * 一个逻辑立体声帧 [L,R] 必须扩展成一个物理四槽帧 [L,0,R,0]：slot0 是
 * 左窗口的有效字，slot1 是左窗口填充，slot2 是右窗口的有效字，slot3 是
 * 右窗口填充。不能写成 [L,R,0,0]，否则 R 位于左窗口的填充区并被忽略，
 * 右窗口开头的 slot2 反而是 0。
 *
 * 寄存器 0x09 当前写 0x0C（0000 1100b）：bit7=0 选择左声道，bit6=0 解除
 * 串行输入静音，bit5=0 使用正常 LRCK 极性，bits4:2=011 选择 16 bit，
 * bits1:0=00 选择 Philips I2S。因此单 DAC 实际只播放 slot0=L，不会自动把
 * L/R 混合；slot2=R 是为了保持标准左右窗口映射。若需两个声道都可听见，
 * 应先由播放器把 L/R 下混为 M，再传入 [M,M]，驱动会展开成 [M,0,M,0]。
 *
 * 本项目固定 16 kHz：BCLK=16000 x 4 x 16=1.024 MHz（64fs），
 * MCLK=16000 x 256=4.096 MHz。播放、录音和识别始终共用这组时钟，播放器
 * 不允许 WAV 文件在运行期间改变采样率。
 *
 * 调用顺序：
 * 1. board_i2c_init() 创建板级共享 I2C0 总线；
 * 2. audio_i2s_init() 启动 TX，持续输出 BCLK 和 WS；
 * 3. 等待时钟稳定后调用 es8311_init()；
 * 4. 最后打开板级功放，播放期间按需调用音量或静音接口。
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
/* 写入复位控制后，等待内部时钟、状态机和模拟电路稳定的时间。 */
#define ES8311_RESET_SETTLE_DELAY_MS 100
/* 上电初始化完成后设置 DAC 默认输出音量为 80%。 */
#define ES8311_DEFAULT_OUTPUT_VOLUME_PERCENT 80U
/* 公开音量接口的百分比上限以及 DAC 音量寄存器的满量程值。 */
#define ES8311_VOLUME_PERCENT_MAX 100U
#define ES8311_DAC_VOLUME_REGISTER_MAX 0xFFU
/* RESET_REG00 的主从模式位：清零后 ES8311 工作在 I2S 从机模式。 */
#define ES8311_MASTER_MODE_MASK 0x40U
/* DAC_REG31 的数字静音控制位域，写 0x60 静音、写 0x00 解除静音。 */
#define ES8311_DAC_MUTE_MASK 0x60U

/* ES8311 寄存器地址。寄存器只在本驱动内部使用，不暴露给应用层。 */
#define ES8311_REG_RESET 0x00            /* 数字电路、状态机和时钟复位。 */
#define ES8311_REG_CLOCK_MANAGER_01 0x01 /* 时钟源选择、极性及总时钟使能。 */
#define ES8311_REG_CLOCK_MANAGER_02 0x02 /* 时钟预分频和预倍频。 */
#define ES8311_REG_CLOCK_MANAGER_03 0x03 /* ADC 速率模式和过采样率。 */
#define ES8311_REG_CLOCK_MANAGER_04 0x04 /* DAC 过采样率。 */
#define ES8311_REG_CLOCK_MANAGER_05 0x05 /* ADC/DAC 工作时钟分频。 */
#define ES8311_REG_CLOCK_MANAGER_06 0x06 /* BCLK 极性和分频。 */
#define ES8311_REG_CLOCK_MANAGER_07 0x07 /* 三态控制、LRCK 分频高位。 */
#define ES8311_REG_CLOCK_MANAGER_08 0x08 /* LRCK 分频低位。 */
#define ES8311_REG_SDP_INPUT 0x09        /* DAC 输入格式、位宽及左右窗口选择。 */
#define ES8311_REG_SDP_OUTPUT 0x0A       /* ADC 串行数据输出格式。 */
#define ES8311_REG_SYSTEM_0B 0x0B       /* 系统控制 0B。 */
#define ES8311_REG_SYSTEM_0C 0x0C       /* 系统控制 0C。 */
#define ES8311_REG_SYSTEM_0D 0x0D       /* 系统上电/掉电控制。 */
#define ES8311_REG_SYSTEM_0E 0x0E       /* 系统电源管理。 */
#define ES8311_REG_SYSTEM_10 0x10       /* 模拟系统上电配置。 */
#define ES8311_REG_SYSTEM_11 0x11       /* 模拟系统上电配置。 */
#define ES8311_REG_SYSTEM_12 0x12       /* DAC 输出通路控制。 */
#define ES8311_REG_SYSTEM_13 0x13       /* 系统模拟通路配置。 */
#define ES8311_REG_SYSTEM_14 0x14       /* 模拟 PGA 和 DMIC 选择。 */
#define ES8311_REG_ADC_15 0x15          /* ADC 斜坡及 DMIC 检测。 */
#define ES8311_REG_ADC_16 0x16          /* ADC 输入和增益配置。 */
#define ES8311_REG_ADC_17 0x17          /* ADC 数字音量。 */
#define ES8311_REG_ADC_1B 0x1B          /* ADC 自动静音和高通滤波。 */
#define ES8311_REG_ADC_1C 0x1C          /* ADC 均衡器和高通滤波。 */
#define ES8311_REG_DAC_31 0x31          /* DAC 数字静音。 */
#define ES8311_REG_DAC_32 0x32          /* DAC 数字音量。 */
#define ES8311_REG_DAC_37 0x37          /* DAC 音量斜坡速率。 */
#define ES8311_REG_GPIO_44 0x44         /* GPIO 和内部测试通路配置。 */
#define ES8311_REG_GPIO_45 0x45         /* 通用控制寄存器。 */

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
 *
 * @param address 8 bit 寄存器地址。
 * @param value 要写入的完整 8 bit 寄存器值。
 * @return ESP_OK 写入成功；否则返回最后一次 I2C 传输错误。
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
 *
 * @param address 8 bit 寄存器地址。
 * @param value 接收寄存器值的有效指针。
 * @return ESP_OK 读取成功；否则返回最后一次 I2C 组合事务错误。
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
 *
 * @param address 需要修改的寄存器地址。
 * @param clear_mask 先从旧值中清除的位。
 * @param set_mask 清除后需要重新置位的位。
 * @return ESP_OK 修改成功；否则返回寄存器读取或写入错误。
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
 * 完整流程：
 * 1. 若已经成功初始化，直接返回，避免重复复位正在工作的 Codec；
 * 2. 探测原理图固定的 7 bit 地址 0x18；
 * 3. 以 100 kHz 把设备加入板级共享 I2C0 总线；
 * 4. 写第一阶段寄存器，建立基础时钟和模拟电源状态并触发内部复位；
 * 5. 等待 100 ms 后清除主模式位，使 ES8311 作为 I2S 从机；
 * 6. 写第二阶段寄存器，选择 BCLK 时钟源，配置 16 bit Philips I2S，并让
 *    单 DAC 选择左声道窗口，再配置 ADC/DAC 信号路径、音量斜坡和 GPIO；
 * 7. 把 DAC 音量改为项目默认的 80%，然后解除数字静音；
 * 8. 任一步失败都移除本次创建的 I2C 设备句柄并恢复未初始化状态。
 *
 * 调用前必须已经初始化 I2C，并让 I2S TX 持续输出稳定的 BCLK/WS。Codec
 * 初始化不负责板级功放使能，功放由 aw9523b 驱动在外层启动流程中打开。
 *
 * @return ESP_OK 初始化完成；否则返回探测、添加设备或寄存器事务错误。
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
     * 第一阶段让时钟树、ADC/DAC 分频和模拟系统进入参考初始状态，最后写
     * RESET_REG00=0x80 触发内部复位。数组顺序属于上电时序的一部分，不能
     * 按寄存器地址排序，也不能把复位写入提前。
     */
    static const es8311_register_value_t initialization[] = {
        {ES8311_REG_GPIO_45, 0x00},            /* 通用控制恢复参考状态。 */
        {ES8311_REG_CLOCK_MANAGER_01, 0x30},  /* 复位前打开基础时钟域。 */
        {ES8311_REG_CLOCK_MANAGER_02, 0x00},  /* 预分频/预倍频恢复初值。 */
        {ES8311_REG_CLOCK_MANAGER_03, 0x10},  /* ADC 过采样参数初值。 */
        {ES8311_REG_ADC_16, 0x24},            /* ADC 模拟输入参考配置。 */
        {ES8311_REG_CLOCK_MANAGER_04, 0x10},  /* DAC 过采样参数初值。 */
        {ES8311_REG_CLOCK_MANAGER_05, 0x00},  /* ADC/DAC 时钟不再分频。 */
        {ES8311_REG_SYSTEM_0B, 0x00},         /* 清除系统控制状态。 */
        {ES8311_REG_SYSTEM_0C, 0x00},         /* 清除系统控制状态。 */
        {ES8311_REG_SYSTEM_10, 0x1F},         /* 建立模拟系统上电状态。 */
        {ES8311_REG_SYSTEM_11, 0x7F},         /* 建立模拟系统上电状态。 */
        {ES8311_REG_RESET, 0x80},             /* 触发数字核心复位。 */
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
    vTaskDelay(pdMS_TO_TICKS(ES8311_RESET_SETTLE_DELAY_MS));

    /* 清除主模式位，同时保留复位寄存器中的其他状态，使 Codec 成为从机。 */
    result = update_register(ES8311_REG_RESET,
                             ES8311_MASTER_MODE_MASK,
                             0x00);
    if (result != ESP_OK) {
        goto init_failed;
    }

    /*
     * 以下时钟和模拟路径值沿用已验证的板级参考配置。当前外部串行总线由
     * ESP32-S3 产生：四个 16 bit 槽使 BCLK=64fs，MCLK 保持 256fs；ES8311
     * 作为从机只在每个 32 BCLK 的 LRCK 半周期读取开头 16 bit，额外 16 bit
     * 是填充时间，不代表 ES8311 被配置成四通道 TDM。
     *
     * 表中同时配置串行格式、单 DAC 输出通路以及参考程序保留的 ADC 状态。
     * REG09=0x0C = 0000 1100b：
     *   bit7     SDP_IN_SEL=0：单 DAC 选择左声道窗口，即物理 slot0；
     *   bit6     SDP_IN_MUTE=0：串行输入解除静音；
     *   bit5     SDP_IN_LRP=0：LRCK 使用正常左右极性；
     *   bits4:2  SDP_IN_WL=011：有效音频字长为 16 bit；
     *   bits1:0  SDP_IN_FMT=00：采用 Philips I2S，MSB 延迟 1 个 BCLK。
     *
     * 上述位宽和格式在播放前已经通过 I2C 写入 Codec。播放时 ES8311 不会
     * 先接收一个 32 bit 整数，再由软件取高/低 16 bit；硬件串行接口直接以
     * LRCK 确定左右窗口，以 BCLK 逐位移入数据。每个 LRCK 半周期虽然有
     * 32 BCLK，但配置的有效字长是 16 bit，所以只把窗口开头的 16 个有效位
     * 组成一个 PCM 样本，剩余 16 BCLK 是共享四槽总线所需的填充时间。
     * bit7=0 又让单 DAC 只采用左窗口样本，因此 [L,0,R,0] 中最终播放的是 L；
     * 如需左右内容都可听见，必须由上层先计算 M=(L+R)/2，再发送 [M,M]。
     * REG14=0x1A 使用模拟 PGA，并关闭数字麦克风输入。
     */
    static const es8311_register_value_t clock_and_signal_path[] = {
        {ES8311_REG_SYSTEM_0D, 0x01},          /* 启动系统工作电源状态。 */
        {ES8311_REG_CLOCK_MANAGER_01, 0xBF},  /* 选择 BCLK 源并启用时钟。 */
        {ES8311_REG_CLOCK_MANAGER_02, 0x18},  /* 预分频 x1、预倍频 x8。 */
        {ES8311_REG_CLOCK_MANAGER_05, 0x00},  /* ADC/DAC 时钟分频均为 x1。 */
        {ES8311_REG_CLOCK_MANAGER_03, 0x10},  /* ADC 单速模式、OSR=16。 */
        {ES8311_REG_CLOCK_MANAGER_04, 0x10},  /* DAC OSR=16。 */
        {ES8311_REG_CLOCK_MANAGER_07, 0x00},  /* 关闭三态，LRCK 分频高位。 */
        {ES8311_REG_CLOCK_MANAGER_08, 0xFF},  /* LRCK 分频低位参考值。 */
        {ES8311_REG_CLOCK_MANAGER_06, 0x03},  /* BCLK 正常极性及分频码。 */
        {ES8311_REG_SDP_INPUT, 0x0C},         /* DAC：16 bit I2S，选择左窗口 slot0。 */
        {ES8311_REG_SDP_OUTPUT, 0x0C},        /* ADC：16 bit Philips I2S。 */
        {ES8311_REG_SYSTEM_14, 0x1A},         /* 使用模拟 PGA，关闭数字麦克风输入。 */
        {ES8311_REG_SYSTEM_12, 0x00},         /* 打开 DAC 输出通路。 */
        {ES8311_REG_SYSTEM_13, 0x10},         /* 配置模拟输出通路。 */
        {ES8311_REG_SYSTEM_0E, 0x02},         /* 系统电源管理参考值。 */
        {ES8311_REG_ADC_15, 0x40},            /* ADC 斜坡/检测参考值。 */
        {ES8311_REG_ADC_1B, 0x0A},            /* ADC 高通滤波参考值 1。 */
        {ES8311_REG_ADC_1C, 0x6A},            /* ADC 高通滤波参考值 2。 */
        {ES8311_REG_DAC_37, 0x48},            /* 配置 DAC 音量变化斜坡。 */
        {ES8311_REG_GPIO_44, 0x08},           /* GPIO 使用正常工作功能。 */
        {ES8311_REG_ADC_17, 0xBF},            /* ADC 数字音量参考值。 */
        {ES8311_REG_DAC_32, 0xBF},            /* 临时 DAC 音量，随后改为 80%。 */
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
                 "Codec ready: 16-bit I2S slave, mono DAC uses left channel, "
                 "volume=%u%%",
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
 * 0x00~0xFF 范围：register = round(percent x 255 / 100)。该百分比是寄存器
 * 刻度，不表示人耳听感线性；0% 也不替代硬件静音，需要完全静音时应调用
 * es8311_set_mute(true)。当前上电默认值为 80%，即寄存器值 0xCC。
 *
 * @param percent DAC 音量百分比，有效范围 0~100。
 * @return ESP_OK 设置成功；ESP_ERR_INVALID_STATE 表示尚未初始化；
 *         ESP_ERR_INVALID_ARG 表示超过 100；其他值为 I2C 写入错误。
 */
esp_err_t es8311_set_volume(uint8_t percent)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent > ES8311_VOLUME_PERCENT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 加 50 后再除以 100，实现整数运算下的四舍五入。 */
    const uint8_t register_value =
        (uint8_t)(((uint16_t)percent * ES8311_DAC_VOLUME_REGISTER_MAX +
                   ES8311_VOLUME_PERCENT_MAX / 2U) /
                  ES8311_VOLUME_PERCENT_MAX);
    return write_register(ES8311_REG_DAC_32, register_value);
}

/**
 * @brief 设置或解除 DAC 数字静音。
 *
 * 仅通过读-改-写更新 DAC_REG31 的 0x60 位域，保留寄存器中与其他 DAC
 * 功能相关的位。该接口不会停止 I2S 时钟、关闭 Codec 或控制外部功放。
 *
 * @param muted true 写入静音位；false 清除静音位并恢复声音输出。
 * @return ESP_OK 设置成功；ESP_ERR_INVALID_STATE 表示尚未初始化；
 *         其他值为 I2C 读取或写入错误。
 */
esp_err_t es8311_set_mute(bool muted)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    return update_register(ES8311_REG_DAC_31,
                           ES8311_DAC_MUTE_MASK,
                           muted ? ES8311_DAC_MUTE_MASK : 0x00);
}
