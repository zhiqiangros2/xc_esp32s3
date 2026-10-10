/**
 * @file es7210.c
 * @brief BOX3 板载 ES7210 四通道麦克风 ADC 驱动。
 *
 * 寄存器配置和时钟系数来自正点原子 21_recoding(ES7210) 例程，并按本工程
 * 的共享 I2C 接口重新实现。与参考代码相比，本驱动会检查全部参数和每次
 * I2C 操作；通信失败最多尝试 5 次，不会无限阻塞录音任务。
 *
 * 从 ES7210 一侧观察，数字音频和模拟输入关系如下：
 *
 *   ESP32 GPIO21 MCLK  --------------------------> MCLK       时钟输入
 *   ESP32 GPIO38 BCLK  --------------------------> SCLK       时钟输入
 *   ESP32 GPIO39 LRCK  --------------------------> LRCK       帧同步输入
 *   ESP32 GPIO41 DIN   <--[MIC1,MIC3,MIC2,MIC4]-- SDOUT1     TDM 输出
 *   ESP32 GPIO40 DOUT  ---> ES8311，与 ES7210 无数字数据连接
 *
 *   环境麦克风 ----------------------------------> MIC1
 *   接地/未使用 ---------------------------------> MIC2
 *   ES8311 OUTP/OUTN 模拟播放信号 --------------> MIC3       AEC 参考
 *   接地/未使用 ---------------------------------> MIC4
 *
 * 四路 ADC 并不各占一根 ESP32 数据线。ES7210 把四个 16 bit 样本按 TDM
 * 时隙顺序复用到唯一的 SDOUT1 上，ESP32 再由 GPIO41 和 RX DMA 还原为四个
 * 连续槽。MCLK/BCLK/LRCK 与 ES8311 共用，但 SDOUT1 是独立的单向录音线。
 *
 * ES7210 是本项目真正的四通道 TDM 器件。它工作在 I2S 从机模式，由
 * ESP32-S3 I2S0 提供 MCLK、BCLK 和 WS/LRCK，四路 ADC 数据经 GPIO41
 *（SDOUT1/TDMOUT）返回 ESP32-S3。默认的 16 bit、1xFS Philips I2S-TDM
 * 每帧固定输出：
 *
 *   slot0      slot1      slot2      slot3
 *   MIC1       MIC3       MIC2       MIC4
 *
 * 板级用途是 MIC1 接环境麦克风，MIC3 接 ES8311 的差分播放输出作为 AEC
 * 参考；MIC2/MIC4 接地，不提供有效音频。录音层仍按当前文件格式要求把
 * MIC1+MIC2 保存成双声道 WAV，因此 WAV 右声道（MIC2）预期接近静音；AEC
 * 则从同一原始帧提取 MIC1+MIC3。驱动保持四路原始槽顺序，不在 Codec 层
 * 重排通道。
 *
 * 16 kHz、16 bit、四槽时，LRCK=16 kHz，BCLK=64fs=1.024 MHz；默认
 * MCLK=256fs=4.096 MHz。MCLK/BCLK/WS 与 ES8311 共用，但录音数据线独立，
 * 因而 ES7210 采集可与 ES8311 播放同时工作。
 */
#include "es7210.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_i2s.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c.h"

/* ES7210 寄存器地址仅供本驱动使用，名称末尾两位是十六进制地址。 */
#define ES7210_RESET_REG00 0x00U          /* 软件复位及芯片运行状态控制。 */
#define ES7210_CLOCK_OFF_REG01 0x01U      /* ADC 模块时钟关闭控制。 */
#define ES7210_MAINCLK_REG02 0x02U        /* ADC 分频、MCLK 倍频及 DLL 旁路控制。 */
#define ES7210_MASTER_CLK_REG03 0x03U     /* MCLK 来源和串行位时钟 SCLK 分频。 */
#define ES7210_LRCK_DIVH_REG04 0x04U      /* LRCK 分频系数的高位。 */
#define ES7210_LRCK_DIVL_REG05 0x05U      /* LRCK 分频系数的低 8 位。 */
#define ES7210_POWER_DOWN_REG06 0x06U     /* 模拟及数字模块的掉电控制。 */
#define ES7210_OSR_REG07 0x07U            /* ADC 过采样率 OSR 配置。 */
#define ES7210_MODE_CONFIG_REG08 0x08U    /* 主从模式及工作通道数量配置。 */
#define ES7210_TIME_CONTROL0_REG09 0x09U  /* 芯片初始化状态周期配置。 */
#define ES7210_TIME_CONTROL1_REG0A 0x0AU  /* 模拟电路上电状态周期配置。 */
#define ES7210_SDP_INTERFACE1_REG11 0x11U /* 音频格式及有效数据位宽配置。 */
#define ES7210_SDP_INTERFACE2_REG12 0x12U /* TDM 模式、1xFS 及数据引脚配置。 */

#define ES7210_ADC_AUTOMUTE_REG13 0x13U   /* ADC 自动静音功能配置。 */
#define ES7210_ADC34_MUTERANGE_REG14 0x14U /* ADC3/4 自动静音检测范围。 */
#define ES7210_ALC_SEL_REG16 0x16U        /* 自动电平控制 ALC 的通道选择。 */
#define ES7210_ADC1_DIRECT_DB_REG1B 0x1BU /* ADC1 数字音量或 ALC 最大增益。 */
#define ES7210_ADC2_DIRECT_DB_REG1C 0x1CU /* ADC2 数字音量或 ALC 最大增益。 */
#define ES7210_ADC3_DIRECT_DB_REG1D 0x1DU /* ADC3 数字音量或 ALC 最大增益。 */
#define ES7210_ADC4_DIRECT_DB_REG1E 0x1EU /* ADC4 数字音量或 ALC 最大增益。 */
#define ES7210_ADC34_HPF2_REG20 0x20U     /* ADC3/4 高通滤波器第二组参数。 */
#define ES7210_ADC34_HPF1_REG21 0x21U     /* ADC3/4 高通滤波器第一组参数。 */
#define ES7210_ADC12_HPF2_REG22 0x22U     /* ADC1/2 高通滤波器第二组参数。 */
#define ES7210_ADC12_HPF1_REG23 0x23U     /* ADC1/2 高通滤波器第一组参数。 */
#define ES7210_ANALOG_REG40 0x40U         /* 模拟电源和内部 VMID 电压配置。 */

#define ES7210_MIC12_BIAS_REG41 0x41U     /* MIC1/2 偏置电压 MICBIAS 配置。 */
#define ES7210_MIC34_BIAS_REG42 0x42U     /* MIC3/4 偏置电压 MICBIAS 配置。 */
#define ES7210_MIC1_GAIN_REG43 0x43U      /* MIC1 PGA 模拟增益配置。 */
#define ES7210_MIC2_GAIN_REG44 0x44U      /* MIC2 PGA 模拟增益配置。 */
#define ES7210_MIC3_GAIN_REG45 0x45U      /* MIC3 PGA 模拟增益配置。 */
#define ES7210_MIC4_GAIN_REG46 0x46U      /* MIC4 PGA 模拟增益配置。 */
#define ES7210_MIC1_POWER_REG47 0x47U     /* MIC1 输入通道电源控制。 */
#define ES7210_MIC2_POWER_REG48 0x48U     /* MIC2 输入通道电源控制。 */
#define ES7210_MIC3_POWER_REG49 0x49U     /* MIC3 输入通道电源控制。 */
#define ES7210_MIC4_POWER_REG4A 0x4AU     /* MIC4 输入通道电源控制。 */
#define ES7210_MIC12_POWER_REG4B 0x4BU    /* MIC1/2 偏置、ADC 和 PGA 电源控制。 */
#define ES7210_MIC34_POWER_REG4C 0x4CU    /* MIC3/4 偏置、ADC 和 PGA 电源控制。 */

/*
 * I2C 参数：
 * - 原理图中 AD1、AD0 均接低电平，7 bit 地址固定为 0x40；
 * - 100 kHz 标准模式给 Codec 上电配置留出充足时序余量；
 * - 单次事务最长等待 100 ms，超时后由下方读写函数决定是否重试。
 */
#define ES7210_I2C_ADDRESS 0x40U
#define ES7210_I2C_FREQUENCY_HZ 100000U
#define ES7210_I2C_TIMEOUT_MS 100
/* 首次操作加四次重试，总尝试次数固定为 5。 */
#define ES7210_I2C_MAX_ATTEMPTS 5U
/* 同时作为失败重试间隔和连续寄存器写入后的稳定时间。 */
#define ES7210_I2C_DELAY_MS 10U

/* 板级默认录音参数直接复用公共 I2S 采样率，防止播放、录音配置不一致。 */
#define ES7210_DEFAULT_SAMPLE_RATE_HZ AUDIO_I2S_SAMPLE_RATE_HZ
#define ES7210_DEFAULT_MCLK_RATIO 256U /* 16 kHz 时 MCLK=4.096 MHz。 */
#define ES7210_DEFAULT_MIC_GAIN ES7210_MIC_GAIN_27_DB /* 麦克风模拟增益。 */
#define ES7210_DEFAULT_VOLUME_DB 0                    /* ADC 数字增益保持 0 dB。 */

/* MIC 增益寄存器高位为参考程序要求的固定控制位，低位保存增益枚举。 */
#define ES7210_MIC_GAIN_FIXED_BITS 0x10U
/*
 * 数字音量有效范围及换算基准：
 *   register = 191 + volume_db x 2
 * 因为寄存器每增加 1 表示增加 0.5 dB，所以整数 dB 需要乘 2。
 */
#define ES7210_MIN_VOLUME_DB (-95)
#define ES7210_MAX_VOLUME_DB 32
#define ES7210_ZERO_DB_REGISTER_VALUE 191

/** 一项“寄存器地址 + 写入值”，用于描述必须按顺序执行的配置表。 */
typedef struct {
    uint8_t address; /* 8 bit ES7210 寄存器地址。 */
    uint8_t value;   /* 要写入该寄存器的完整 8 bit 值。 */
} es7210_register_value_t;

/**
 * ES7210 内部时钟树参数。lrck_div_high/low 合起来是 LRCK 分频值；
 * MAINCLK 寄存器最终由 adc_div、DLL bypass 和倍频使能位组合得到：
 *   bit[5:0] = adc_div，bit6 = doubler_enabled，bit7 = dll_bypass。
 */
typedef struct {
    uint32_t mclk_hz;        /* ESP32-S3 实际输出的主时钟频率。 */
    uint32_t sample_rate_hz; /* WS/LRCK 频率，即每声道采样率。 */
    uint8_t adc_div;         /* MAINCLK[5:0]：ADC 内部时钟分频值。 */
    uint8_t dll_bypass;      /* MAINCLK[7]：1 表示旁路 DLL。 */
    uint8_t doubler_enabled; /* MAINCLK[6]：1 表示启用时钟倍频。 */
    uint8_t osr;             /* OSR 寄存器：ADC 过采样率。 */
    uint8_t lrck_div_high;   /* LRCK 分频系数高位。 */
    uint8_t lrck_div_low;    /* LRCK 分频系数低 8 位。 */
} es7210_clock_coefficient_t;

/*
 * 正点原子参考驱动支持的完整时钟表。只有 MCLK 和采样率同时匹配的项目
 * 才能使用，避免用近似分频产生错误采样率。
 *
 * 每行字段顺序：
 *   MCLK, 采样率, ADC分频, DLL旁路, 倍频使能, OSR, LRCK高位, LRCK低位
 *
 * 同一采样率可能有多个 MCLK 方案。调用者传入 sample_rate_hz 和
 * mclk_ratio，驱动先计算 MCLK = sample_rate_hz x mclk_ratio，再精确查表。
 */
static const es7210_clock_coefficient_t s_clock_coefficients[] = {
    /* 8 kHz */
    {12288000U, 8000U, 0x03, 0x01, 0x00, 0x20, 0x06, 0x00},
    {16384000U, 8000U, 0x04, 0x01, 0x00, 0x20, 0x08, 0x00},
    {19200000U, 8000U, 0x1E, 0x00, 0x01, 0x28, 0x09, 0x60},
    {4096000U, 8000U, 0x01, 0x01, 0x00, 0x20, 0x02, 0x00},

    /* 11.025 kHz */
    {11289600U, 11025U, 0x02, 0x01, 0x00, 0x20, 0x01, 0x00},

    /* 12 kHz */
    {12288000U, 12000U, 0x02, 0x01, 0x00, 0x20, 0x04, 0x00},
    {19200000U, 12000U, 0x14, 0x00, 0x01, 0x28, 0x06, 0x40},

    /* 16 kHz */
    {4096000U, 16000U, 0x01, 0x01, 0x01, 0x20, 0x01, 0x00},
    {19200000U, 16000U, 0x0A, 0x00, 0x00, 0x1E, 0x04, 0x80},
    {16384000U, 16000U, 0x02, 0x01, 0x00, 0x20, 0x04, 0x00},
    {12288000U, 16000U, 0x03, 0x01, 0x01, 0x20, 0x03, 0x00},

    /* 22.05 kHz */
    {11289600U, 22050U, 0x01, 0x01, 0x00, 0x20, 0x02, 0x00},

    /* 24 kHz */
    {12288000U, 24000U, 0x01, 0x01, 0x00, 0x20, 0x02, 0x00},
    {19200000U, 24000U, 0x0A, 0x00, 0x01, 0x28, 0x03, 0x20},

    /* 32 kHz */
    {12288000U, 32000U, 0x03, 0x00, 0x00, 0x20, 0x01, 0x80},
    {16384000U, 32000U, 0x01, 0x01, 0x00, 0x20, 0x02, 0x00},
    {19200000U, 32000U, 0x05, 0x00, 0x00, 0x1E, 0x02, 0x58},

    /* 44.1 kHz */
    {11289600U, 44100U, 0x01, 0x01, 0x01, 0x20, 0x01, 0x00},

    /* 48 kHz */
    {12288000U, 48000U, 0x01, 0x01, 0x01, 0x20, 0x01, 0x00},
    {19200000U, 48000U, 0x05, 0x00, 0x01, 0x28, 0x01, 0x90},

    /* 64 kHz */
    {16384000U, 64000U, 0x01, 0x01, 0x00, 0x20, 0x01, 0x00},
    {19200000U, 64000U, 0x05, 0x00, 0x01, 0x1E, 0x01, 0x2C},

    /* 88.2 kHz */
    {11289600U, 88200U, 0x01, 0x01, 0x01, 0x20, 0x00, 0x80},

    /* 96 kHz */
    {12288000U, 96000U, 0x01, 0x01, 0x01, 0x20, 0x00, 0x80},
    {19200000U, 96000U, 0x05, 0x00, 0x01, 0x28, 0x00, 0xC8},
};

/* 挂接在板级共享 I2C0 总线上的 ES7210 设备句柄。 */
static board_i2c_device_handle_t s_device;
/* 只有完整配置和关键寄存器回读均成功后才置 true。 */
static bool s_initialized;
/* ESP-IDF 日志标签。 */
static const char *TAG = "ES7210";

/**
 * @brief 通过 I2C 写入一个 ES7210 寄存器。
 *
 * I2C 发送内容固定为两个字节：[寄存器地址, 寄存器值]。一次首次发送加
 * 四次重试，总共最多 5 次；只有前四次失败后才等待 10 ms，第五次失败
 * 立即把错误返回上层，不像参考程序那样永久循环。
 *
 * @param address 目标寄存器地址。
 * @param value 写入的完整 8 bit 寄存器值。
 * @return ESP_OK 写入成功；ESP_ERR_INVALID_STATE 表示设备尚未挂接；
 *         其他错误来自板级 I2C 驱动。
 */
static esp_err_t write_register(uint8_t address, uint8_t value)
{
    if (s_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* ES7210 的写事务不需要单独发送数据长度或校验字节。 */
    const uint8_t data[] = {address, value};
    esp_err_t result = ESP_FAIL;
    /* attempt 的取值为 1 到 5，因此不会出现无限重试。 */
    for (unsigned int attempt = 1;
         attempt <= ES7210_I2C_MAX_ATTEMPTS;
         ++attempt) {
        result = board_i2c_transmit(s_device,
                                    data,
                                    sizeof(data),
                                    ES7210_I2C_TIMEOUT_MS);
        if (result == ESP_OK) {
            return ESP_OK;
        }

        /* 最后一次已经没有下一次重试，不能再做无意义等待。 */
        if (attempt < ES7210_I2C_MAX_ATTEMPTS) {
            ESP_LOGW(TAG,
                     "Write reg 0x%02X failed (%u/%u): %s",
                     address,
                     attempt,
                     ES7210_I2C_MAX_ATTEMPTS,
                     esp_err_to_name(result));
            vTaskDelay(pdMS_TO_TICKS(ES7210_I2C_DELAY_MS));
        }
    }

    ESP_LOGE(TAG,
             "Write reg 0x%02X failed after %u attempts: %s",
             address,
             ES7210_I2C_MAX_ATTEMPTS,
             esp_err_to_name(result));
    return result;
}

/**
 * @brief 读取一个 ES7210 寄存器。
 *
 * board_i2c_transmit_receive() 在同一事务中先发送寄存器地址，再用重复
 * START 读取 1 字节，避免两次独立事务之间被其他 I2C 设备插入。
 * 读取同样最多尝试 5 次，value 只在返回 ESP_OK 时有效。
 *
 * @param address 目标寄存器地址。
 * @param value 返回读取到的 8 bit 寄存器值。
 * @return ESP_OK 读取成功；ESP_ERR_INVALID_ARG 输出指针为空；
 *         ESP_ERR_INVALID_STATE 设备未挂接；或底层 I2C 错误。
 */
static esp_err_t read_register(uint8_t address, uint8_t *value)
{
    if (s_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_FAIL;
    for (unsigned int attempt = 1;
         attempt <= ES7210_I2C_MAX_ATTEMPTS;
         ++attempt) {
        result = board_i2c_transmit_receive(s_device,
                                            &address,
                                            sizeof(address),
                                            value,
                                            sizeof(*value),
                                            ES7210_I2C_TIMEOUT_MS);
        if (result == ESP_OK) {
            return ESP_OK;
        }

        if (attempt < ES7210_I2C_MAX_ATTEMPTS) {
            ESP_LOGW(TAG,
                     "Read reg 0x%02X failed (%u/%u): %s",
                     address,
                     attempt,
                     ES7210_I2C_MAX_ATTEMPTS,
                     esp_err_to_name(result));
            vTaskDelay(pdMS_TO_TICKS(ES7210_I2C_DELAY_MS));
        }
    }

    ESP_LOGE(TAG,
             "Read reg 0x%02X failed after %u attempts: %s",
             address,
             ES7210_I2C_MAX_ATTEMPTS,
             esp_err_to_name(result));
    return result;
}

/**
 * @brief 写寄存器并等待器件内部状态稳定。
 *
 * 这个等待只在写入成功后执行。写入失败时错误立即向上传递，避免失败路径
 * 再额外等待。初始化表和运行时音量设置统一通过该函数保持相同时序。
 *
 * @param address 目标寄存器地址。
 * @param value 写入值。
 * @return write_register() 的结果。
 */
static esp_err_t write_register_and_wait(uint8_t address, uint8_t value)
{
    const esp_err_t result = write_register(address, value);
    if (result == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(ES7210_I2C_DELAY_MS));
    }
    return result;
}

/**
 * @brief 按数组顺序写入一组寄存器。
 *
 * 数组顺序就是 Codec 的配置时序。任何一项失败都会立即停止，后续项目
 * 不再写入，调用者因此不会把“部分配置成功”误认为初始化完成。
 *
 * @param sequence 寄存器配置数组。
 * @param sequence_length 数组元素数量，不是字节数。
 * @return ESP_OK 全部写入成功；ESP_ERR_INVALID_ARG 数组无效；
 *         或首个失败写操作返回的错误。
 */
static esp_err_t write_register_sequence(
    const es7210_register_value_t *sequence,
    size_t sequence_length)
{
    if (sequence == NULL || sequence_length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t index = 0; index < sequence_length; ++index) {
        const esp_err_t result =
            write_register_and_wait(sequence[index].address,
                                    sequence[index].value);
        if (result != ESP_OK) {
            ESP_LOGE(TAG,
                     "Register sequence stopped at index %u (reg 0x%02X)",
                     (unsigned int)index,
                     sequence[index].address);
            return result;
        }
    }
    return ESP_OK;
}

/**
 * @brief 回读并校验一个寄存器。
 *
 * I2C 地址有应答并不代表配置一定生效，因此初始化结束后要检查关键寄存器。
 * 总线读取失败时保留原错误；读取成功但数值不一致时返回
 * ESP_ERR_INVALID_RESPONSE。
 *
 * @param address 要校验的寄存器地址。
 * @param expected 期望读到的值。
 * @return ESP_OK 校验一致；否则返回读取或数值校验错误。
 */
static esp_err_t verify_register(uint8_t address, uint8_t expected)
{
    uint8_t actual = 0;
    esp_err_t result = read_register(address, &actual);
    if (result == ESP_OK && actual != expected) {
        ESP_LOGE(TAG,
                 "Reg 0x%02X mismatch: expected 0x%02X, got 0x%02X",
                 address,
                 expected,
                 actual);
        result = ESP_ERR_INVALID_RESPONSE;
    }
    return result;
}

/**
 * @brief 按 MCLK 和采样率精确查找内部时钟参数。
 *
 * 不做最接近值匹配，因为近似分频会使 WAV 文件头中的采样率与实际采样率
 * 不一致，导致播放速度和时长错误。
 *
 * @param mclk_hz ESP32-S3 输出的 MCLK 频率。
 * @param sample_rate_hz 目标 WS/LRCK 频率。
 * @return 匹配表项的只读指针；没有支持的组合时返回 NULL。
 */
static const es7210_clock_coefficient_t *find_clock_coefficient(
    uint32_t mclk_hz,
    uint32_t sample_rate_hz)
{
    for (size_t index = 0;
         index < sizeof(s_clock_coefficients) /
                     sizeof(s_clock_coefficients[0]);
         ++index) {
        if (s_clock_coefficients[index].mclk_hz == mclk_hz &&
            s_clock_coefficients[index].sample_rate_hz ==
                sample_rate_hz) {
            return &s_clock_coefficients[index];
        }
    }
    return NULL;
}

/**
 * @brief 把公开枚举转换为串行接口寄存器 0x11 和 0x12。
 *
 * 寄存器 0x11 由“有效位宽编码 | 音频格式编码”组成。寄存器 0x12 在
 * 非 TDM 模式下固定为 0；TDM 模式下 I2S/左对齐使用 0x02，
 * DSP-A/DSP-B 使用 0x01。
 *
 * @param config 用户提供的 Codec 配置。
 * @param interface1 返回寄存器 0x11 的完整值。
 * @param interface2 返回寄存器 0x12 的完整值。
 * @return ESP_OK 转换成功；ESP_ERR_INVALID_ARG 枚举值不受支持。
 */
static esp_err_t get_interface_register_values(
    const es7210_codec_config_t *config,
    uint8_t *interface1,
    uint8_t *interface2)
{
    /* 位宽编码占寄存器 0x11 的高三位。 */
    uint8_t width_value = 0;
    switch (config->bit_width) {
        case ES7210_I2S_BITS_16:
            width_value = 0x60;
            break;
        case ES7210_I2S_BITS_18:
            width_value = 0x40;
            break;
        case ES7210_I2S_BITS_20:
            width_value = 0x20;
            break;
        case ES7210_I2S_BITS_24:
            width_value = 0x00;
            break;
        case ES7210_I2S_BITS_32:
            width_value = 0x80;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }

    /*
     * 格式枚举值本身就是寄存器 0x11 的低位编码。开启 TDM 时，0x12=0x02
     * 配置 I2S/左对齐的 1xFS TDM；单芯片 SDOUT1 随后按
     * [MIC1,MIC3,MIC2,MIC4] 输出。DSP A/B 使用其对应的 0x01 TDM 模式。
     */
    uint8_t tdm_value = 0;
    switch (config->i2s_format) {
        case ES7210_I2S_FMT_I2S:
        case ES7210_I2S_FMT_LEFT_JUSTIFIED:
            tdm_value = 0x02;
            break;
        case ES7210_I2S_FMT_DSP_A:
        case ES7210_I2S_FMT_DSP_B:
            tdm_value = 0x01;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }

    *interface1 = width_value | (uint8_t)config->i2s_format;
    *interface2 = config->tdm_enabled ? tdm_value : 0x00;
    return ESP_OK;
}

/**
 * @brief 一次完成配置参数校验和时钟表查找。
 *
 * 校验顺序有意放在任何寄存器写入之前。这样非法参数不会先复位 Codec，
 * 也不会破坏当前正在使用的有效配置。
 *
 * @param config 待检查的公开配置。
 * @param coefficient 返回匹配的只读时钟表项。
 * @param interface1 返回寄存器 0x11 的预计算值。
 * @param interface2 返回寄存器 0x12 的预计算值。
 * @return ESP_OK 参数完整有效；ESP_ERR_INVALID_ARG 参数或枚举非法；
 *         ESP_ERR_NOT_SUPPORTED 没有匹配的 MCLK/采样率组合。
 */
static esp_err_t validate_config(
    const es7210_codec_config_t *config,
    const es7210_clock_coefficient_t **coefficient,
    uint8_t *interface1,
    uint8_t *interface2)
{
    if (config == NULL || coefficient == NULL ||
        interface1 == NULL || interface2 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * 除了排除 0，还先检查乘法是否会超过 uint32_t，之后计算 MCLK 才安全。
     */
    if (config->sample_rate_hz == 0 || config->mclk_ratio == 0 ||
        config->sample_rate_hz > UINT32_MAX / config->mclk_ratio) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((unsigned int)config->mic_gain >
        (unsigned int)ES7210_MIC_GAIN_37_5_DB) {
        return ESP_ERR_INVALID_ARG;
    }
    if (((uint8_t)config->mic_bias & 0x0FU) != 0 ||
        (uint8_t)config->mic_bias > (uint8_t)ES7210_MIC_BIAS_2V87) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result =
        get_interface_register_values(config, interface1, interface2);
    if (result != ESP_OK) {
        return result;
    }

    /* ESP32-S3 的 MCLK 频率由采样率和倍频比共同决定。 */
    const uint32_t mclk_hz =
        config->sample_rate_hz * config->mclk_ratio;
    *coefficient =
        find_clock_coefficient(mclk_hz, config->sample_rate_hz);
    if (*coefficient == NULL) {
        ESP_LOGE(TAG,
                 "Unsupported clock: sample=%" PRIu32
                 " Hz, ratio=%" PRIu32 ", MCLK=%" PRIu32 " Hz",
                 config->sample_rate_hz,
                 config->mclk_ratio,
                 mclk_hz);
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

/**
 * @brief 把一个时钟表项写入 ES7210 时钟树寄存器。
 *
 * 写入顺序是 OSR -> MAINCLK -> LRCK 高位 -> LRCK 低位。MAINCLK 的
 * bit7/bit6 分别由 DLL 旁路和倍频标志生成，低六位保存 ADC 分频值。
 *
 * @param coefficient 已由 find_clock_coefficient() 找到的有效表项。
 * @return ESP_OK 配置成功；或首个失败寄存器写入返回的错误。
 */
static esp_err_t configure_clock(
    const es7210_clock_coefficient_t *coefficient)
{
    const uint8_t main_clock =
        coefficient->adc_div |
        (uint8_t)(coefficient->doubler_enabled << 6) |
        (uint8_t)(coefficient->dll_bypass << 7);
    const es7210_register_value_t clock_sequence[] = {
        {ES7210_OSR_REG07, coefficient->osr},
        {ES7210_MAINCLK_REG02, main_clock},
        {ES7210_LRCK_DIVH_REG04, coefficient->lrck_div_high},
        {ES7210_LRCK_DIVL_REG05, coefficient->lrck_div_low},
    };
    return write_register_sequence(
        clock_sequence,
        sizeof(clock_sequence) / sizeof(clock_sequence[0]));
}

/**
 * @brief 给两组麦克风写入相同的 MICBIAS 电压。
 *
 * ES7210 使用两个独立寄存器分别控制 MIC1/2 与 MIC3/4，本项目四路采用
 * 同一配置，避免不同通道直流工作点不一致。
 *
 * @param mic_bias 已校验的 MICBIAS 枚举值。
 * @return ESP_OK 成功；或 I2C 写入错误。
 */
static esp_err_t configure_mic_bias(es7210_mic_bias_t mic_bias)
{
    const es7210_register_value_t sequence[] = {
        {ES7210_MIC12_BIAS_REG41, (uint8_t)mic_bias},
        {ES7210_MIC34_BIAS_REG42, (uint8_t)mic_bias},
    };
    return write_register_sequence(sequence,
                                   sizeof(sequence) /
                                       sizeof(sequence[0]));
}

/**
 * @brief 给四路麦克风 PGA 写入相同的模拟增益。
 *
 * 0x10 是参考初始化要求保留的控制位，低四位使用增益枚举值。这里配置的是
 * ADC 前端模拟增益，与寄存器 0x1B～0x1E 的 ADC 数字音量不同。
 *
 * @param mic_gain 已校验的模拟增益枚举值。
 * @return ESP_OK 成功；或 I2C 写入错误。
 */
static esp_err_t configure_mic_gain(es7210_mic_gain_t mic_gain)
{
    const uint8_t register_value =
        ES7210_MIC_GAIN_FIXED_BITS | (uint8_t)mic_gain;
    const es7210_register_value_t sequence[] = {
        {ES7210_MIC1_GAIN_REG43, register_value},
        {ES7210_MIC2_GAIN_REG44, register_value},
        {ES7210_MIC3_GAIN_REG45, register_value},
        {ES7210_MIC4_GAIN_REG46, register_value},
    };
    return write_register_sequence(sequence,
                                   sizeof(sequence) /
                                       sizeof(sequence[0]));
}

/**
 * @brief 回读影响录音格式和信号增益的关键寄存器。
 *
 * 校验复位终态、串行接口、完整时钟链、两组 MICBIAS，以及第一和第四路
 * PGA 增益。它既覆盖配置链两端，也避免把每个固定电源寄存器全部重复读取
 * 而显著延长启动时间。
 *
 * @param config 本次已经写入的公开配置。
 * @param coefficient 本次使用的时钟表项。
 * @param interface1 寄存器 0x11 的期望值。
 * @param interface2 寄存器 0x12 的期望值。
 * @return ESP_OK 全部一致；否则返回首个读取或数值校验错误。
 */
static esp_err_t verify_codec_configuration(
    const es7210_codec_config_t *config,
    const es7210_clock_coefficient_t *coefficient,
    uint8_t interface1,
    uint8_t interface2)
{
    const uint8_t main_clock =
        coefficient->adc_div |
        (uint8_t)(coefficient->doubler_enabled << 6) |
        (uint8_t)(coefficient->dll_bypass << 7);
    const uint8_t mic_gain =
        ES7210_MIC_GAIN_FIXED_BITS | (uint8_t)config->mic_gain;
    const es7210_register_value_t expected[] = {
        {ES7210_RESET_REG00, 0x41},
        {ES7210_SDP_INTERFACE1_REG11, interface1},
        {ES7210_SDP_INTERFACE2_REG12, interface2},
        {ES7210_OSR_REG07, coefficient->osr},
        {ES7210_MAINCLK_REG02, main_clock},
        {ES7210_LRCK_DIVH_REG04, coefficient->lrck_div_high},
        {ES7210_LRCK_DIVL_REG05, coefficient->lrck_div_low},
        {ES7210_MIC12_BIAS_REG41, (uint8_t)config->mic_bias},
        {ES7210_MIC34_BIAS_REG42, (uint8_t)config->mic_bias},
        {ES7210_MIC1_GAIN_REG43, mic_gain},
        {ES7210_MIC4_GAIN_REG46, mic_gain},
    };

    for (size_t index = 0;
         index < sizeof(expected) / sizeof(expected[0]);
         ++index) {
        const esp_err_t result =
            verify_register(expected[index].address,
                            expected[index].value);
        if (result != ESP_OK) {
            return result;
        }
    }
    return ESP_OK;
}

/**
 * @brief 按指定参数重新配置 ES7210 的完整录音链路。
 *
 * 本函数会软件复位 Codec，并依次设置启动时序、高通滤波器、串行接口、
 * 模拟输入、内部时钟和通道电源。全部写入完成后还会回读关键寄存器。
 *
 * 调用要求：
 * 1. es7210_init() 已把设备挂接到板级 I2C 总线；
 * 2. ESP32-S3 已输出与 config 匹配的稳定 MCLK/BCLK/WS；
 * 3. I2S RX 当前未读取数据，因为重新配置会短暂关闭 ADC。
 *
 * @param config 采样率、MCLK 倍频比、接口格式、位宽、偏置和增益配置。
 * @return ESP_OK 配置及校验成功；否则返回状态、参数、时钟或 I2C 错误。
 */
esp_err_t es7210_config_codec(const es7210_codec_config_t *config)
{
    if (s_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const es7210_clock_coefficient_t *coefficient = NULL;
    uint8_t interface1 = 0;
    uint8_t interface2 = 0;
    esp_err_t result = validate_config(config,
                                       &coefficient,
                                       &interface1,
                                       &interface2);
    if (result != ESP_OK) {
        return result;
    }

    /*
     * 固定启动段来自参考例程，数组先后顺序不能交换：
     * 1. 0xFF -> 0x32 让芯片进入可配置的复位后状态；
     * 2. 两个 0x30 设置模拟模块初始化及上电状态周期；
     * 3. 0x2A/0x0A 为两组 ADC 使用相同的高通滤波器参数。
     */
    static const es7210_register_value_t reset_and_hpf[] = {
        {ES7210_RESET_REG00, 0xFF},         /* 触发软件复位。 */
        {ES7210_RESET_REG00, 0x32},         /* 进入寄存器配置状态。 */
        {ES7210_TIME_CONTROL0_REG09, 0x30}, /* 初始化状态周期。 */
        {ES7210_TIME_CONTROL1_REG0A, 0x30}, /* 模拟上电状态周期。 */
        {ES7210_ADC12_HPF1_REG23, 0x2A},    /* ADC1/2 HPF 参数 1。 */
        {ES7210_ADC12_HPF2_REG22, 0x0A},    /* ADC1/2 HPF 参数 2。 */
        {ES7210_ADC34_HPF1_REG21, 0x2A},    /* ADC3/4 HPF 参数 1。 */
        {ES7210_ADC34_HPF2_REG20, 0x0A},    /* ADC3/4 HPF 参数 2。 */
    };
    /* 0x08 是参考程序用于开启各麦克风模拟输入通道的电源状态。 */
    static const es7210_register_value_t mic_power[] = {
        {ES7210_MIC1_POWER_REG47, 0x08}, /* 开启 MIC1 输入通道。 */
        {ES7210_MIC2_POWER_REG48, 0x08}, /* 开启 MIC2 输入通道。 */
        {ES7210_MIC3_POWER_REG49, 0x08}, /* 开启 MIC3 输入通道。 */
        {ES7210_MIC4_POWER_REG4A, 0x08}, /* 开启 MIC4 输入通道。 */
    };
    static const es7210_register_value_t start_adc[] = {
        {ES7210_POWER_DOWN_REG06, 0x04},  /* 按参考配置关闭 DLL。 */
        {ES7210_MIC12_POWER_REG4B, 0x0F}, /* 开启 MIC1/2 偏置、ADC、PGA。 */
        {ES7210_MIC34_POWER_REG4C, 0x0F}, /* 开启 MIC3/4 偏置、ADC、PGA。 */
        /* 0x71 -> 0x41 是参考驱动规定的最终使能序列。 */
        {ES7210_RESET_REG00, 0x71}, /* 准备从配置状态切换到运行状态。 */
        {ES7210_RESET_REG00, 0x41}, /* 进入正常 ADC 运行状态。 */
    };

    /*
     * 一旦开始写复位序列，旧配置就不再保证有效。先清除初始化标志，使本次
     * 任意一步失败后 es7210_config_volume() 不会继续修改部分配置的芯片。
     */
    s_initialized = false;
    /* 阶段 1：软件复位、上电时间及四路 ADC 高通滤波器。 */
    result = write_register_sequence(
        reset_and_hpf,
        sizeof(reset_and_hpf) / sizeof(reset_and_hpf[0]));
    /*
     * 阶段 2：有效位宽、I2S/DSP 格式及 TDM 输出模式。默认值会写入
     * 0x11=0x60（16 bit Philips I2S）和 0x12=0x02（1xFS 四通道 TDM），
     * 使 GPIO41 每帧输出 [MIC1,MIC3,MIC2,MIC4]。
     */
    if (result == ESP_OK) {
        result = write_register_and_wait(ES7210_SDP_INTERFACE1_REG11,
                                         interface1);
    }
    if (result == ESP_OK) {
        result = write_register_and_wait(ES7210_SDP_INTERFACE2_REG12,
                                         interface2);
    }
    /* 阶段 3：打开模拟电源并建立内部 VMID 工作点。 */
    if (result == ESP_OK) {
        result = write_register_and_wait(ES7210_ANALOG_REG40, 0xC3);
    }
    /* 阶段 4：设置两组麦克风偏置和四路 PGA 模拟增益。 */
    if (result == ESP_OK) {
        result = configure_mic_bias(config->mic_bias);
    }
    if (result == ESP_OK) {
        result = configure_mic_gain(config->mic_gain);
    }
    /* 阶段 5：开启四路麦克风模拟输入通道。 */
    if (result == ESP_OK) {
        result = write_register_sequence(
            mic_power,
            sizeof(mic_power) / sizeof(mic_power[0]));
    }
    /* 阶段 6：按精确时钟表设置 OSR、ADC 分频和 LRCK 分频。 */
    if (result == ESP_OK) {
        result = configure_clock(coefficient);
    }
    /* 阶段 7：开启 MICBIAS/ADC/PGA，并执行最终运行使能序列。 */
    if (result == ESP_OK) {
        result = write_register_sequence(
            start_adc,
            sizeof(start_adc) / sizeof(start_adc[0]));
    }
    /* 阶段 8：回读关键配置，确认不是仅有 I2C ACK 而实际写入丢失。 */
    if (result == ESP_OK) {
        result = verify_codec_configuration(config,
                                            coefficient,
                                            interface1,
                                            interface2);
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "Codec configuration failed: %s",
                 esp_err_to_name(result));
        return result;
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "Configured: sample=%" PRIu32
             " Hz, MCLK=%" PRIu32
             " Hz, %u-bit, TDM=%s, gain=%u",
             config->sample_rate_hz,
             config->sample_rate_hz * config->mclk_ratio,
             (unsigned int)config->bit_width,
             config->tdm_enabled ? "on" : "off",
             (unsigned int)config->mic_gain);
    return ESP_OK;
}

/**
 * @brief 同时设置 ADC1～ADC4 的数字音量。
 *
 * ES7210 的四个 ADC 各有独立音量寄存器。本项目统一设置四路，写完后逐路
 * 回读。该数字增益位于 ADC 转换之后，不替代 configure_mic_gain() 设置的
 * 模拟 PGA 增益。
 *
 * @param volume_db 整数数字增益，范围 -95～+32 dB。
 * @return ESP_OK 四路写入并校验成功；或状态、参数、I2C/校验错误。
 */
esp_err_t es7210_config_volume(int8_t volume_db)
{
    if (!s_initialized || s_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (volume_db < ES7210_MIN_VOLUME_DB ||
        volume_db > ES7210_MAX_VOLUME_DB) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 数据手册的 ADC 数字音量步进为 0.5 dB，0 dB 对应 191；
     * 当前 API 使用整数 dB，因此每增加 1 dB，寄存器增加 2。
     */
    /*
     * 示例：
     *   -95 dB -> 191 - 190 = 1；
     *     0 dB -> 191；
     *   +32 dB -> 191 + 64 = 255 (0xFF)。
     */
    const int register_value =
        ES7210_ZERO_DB_REGISTER_VALUE + (int)volume_db * 2;
    const uint8_t value = (uint8_t)register_value;
    const es7210_register_value_t sequence[] = {
        {ES7210_ADC1_DIRECT_DB_REG1B, value},
        {ES7210_ADC2_DIRECT_DB_REG1C, value},
        {ES7210_ADC3_DIRECT_DB_REG1D, value},
        {ES7210_ADC4_DIRECT_DB_REG1E, value},
    };

    esp_err_t result =
        write_register_sequence(sequence,
                                sizeof(sequence) /
                                    sizeof(sequence[0]));
    if (result != ESP_OK) {
        return result;
    }

    /* 四路都回读，避免只校验第一路而遗漏中间某次写失败。 */
    for (size_t index = 0;
         index < sizeof(sequence) / sizeof(sequence[0]);
         ++index) {
        result = verify_register(sequence[index].address, value);
        if (result != ESP_OK) {
            return result;
        }
    }

    ESP_LOGI(TAG,
             "ADC digital volume: %d dB (register 0x%02X)",
             (int)volume_db,
             value);
    return ESP_OK;
}

/**
 * @brief 使用板级默认参数初始化 ES7210。
 *
 * 完整流程：
 * 1. 已初始化则直接返回，保证接口可重复调用；
 * 2. 在固定地址 0x40 探测器件，再加入板级共享 I2C0 总线；
 * 3. 配置 16 kHz、16 bit、1xFS 四槽 Philips I2S-TDM、MICBIAS 和增益；
 * 4. 把四路 ADC 数字音量设置为 0 dB，避免数字增益导致采样削顶；
 * 5. 任一步失败都移除本驱动创建的设备句柄，使后续调用可重新开始。
 *
 * 调用本函数之前，board_i2c_init() 和 audio_i2s_init() 必须已经成功，
 * 而且 I2S TX 必须持续输出 Codec 所需的 MCLK/BCLK/WS。
 *
 * @return ESP_OK 初始化成功；否则返回探测、配置、校验或清理前的原始错误。
 */
esp_err_t es7210_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t result = ESP_OK;
    if (s_device == NULL) {
        /* 先探测再添加设备，地址错误或硬件未上电时日志更明确。 */
        result = board_i2c_probe(ES7210_I2C_ADDRESS,
                                 ES7210_I2C_TIMEOUT_MS);
        if (result != ESP_OK) {
            ESP_LOGE(TAG,
                     "No response at fixed I2C address 0x%02X: %s",
                     ES7210_I2C_ADDRESS,
                     esp_err_to_name(result));
            return result;
        }

        /* 为固定地址创建句柄，后续所有事务由板级 I2C 互斥锁串行化。 */
        result = board_i2c_add_device(ES7210_I2C_ADDRESS,
                                      ES7210_I2C_FREQUENCY_HZ,
                                      &s_device);
        if (result != ESP_OK) {
            s_device = NULL;
            return result;
        }
    }

    /*
     * 与 audio_i2s.c 的 16 kHz、16 bit、四槽 Philips TDM 配套。启用寄存器
     * 0x12 的 1xFS I2S-TDM 后，板上 GPIO41 连接的 SDOUT1/TDMOUT 在每个
     * LRCK 周期依次输出 [MIC1,MIC3,MIC2,MIC4]：槽 0/1 位于 LRCK 低半周，
     * 槽 2/3 位于高半周。录音任务从槽 0/2 保存 MIC1+MIC2 WAV，同时从槽
     * 0/1 提取同步的 MIC1+MIC3 供 AEC 使用。
     */
    static const es7210_codec_config_t default_config = {
        .sample_rate_hz = ES7210_DEFAULT_SAMPLE_RATE_HZ, /* 16 kHz。 */
        .mclk_ratio = ES7210_DEFAULT_MCLK_RATIO,         /* MCLK=4.096 MHz。 */
        .i2s_format = ES7210_I2S_FMT_I2S,                /* Philips I2S。 */
        .bit_width = ES7210_I2S_BITS_16,                 /* 每采样 16 bit。 */
        .mic_bias = ES7210_MIC_BIAS_2V87,                /* 最大 MICBIAS。 */
        .mic_gain = ES7210_DEFAULT_MIC_GAIN,             /* 27 dB，适度提高录音电平。 */
        .tdm_enabled = true,                             /* SDOUT1 输出全部四路 ADC。 */
    };

    /* 先建立完整模拟/数字录音链路，再设置独立的 ADC 数字音量。 */
    result = es7210_config_codec(&default_config);
    if (result == ESP_OK) {
        result = es7210_config_volume(ES7210_DEFAULT_VOLUME_DB);
    }
    if (result == ESP_OK) {
        ESP_LOGI(TAG,
                 "ADC ready at fixed I2C address 0x%02X",
                 ES7210_I2C_ADDRESS);
        return ESP_OK;
    }

    /*
     * 失败时释放设备句柄。保留最初的 result 返回给调用者，清理失败只记录
     * 警告，避免真正的初始化失败原因被 board_i2c_remove_device() 覆盖。
     */
    s_initialized = false;
    const esp_err_t remove_result =
        board_i2c_remove_device(s_device);
    if (remove_result != ESP_OK) {
        ESP_LOGW(TAG,
                 "Cannot remove failed device handle: %s",
                 esp_err_to_name(remove_result));
    }
    s_device = NULL;
    return result;
}
