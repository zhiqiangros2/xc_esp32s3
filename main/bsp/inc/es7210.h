#ifndef BOARD_ES7210_H
#define BOARD_ES7210_H

/**
 * @file es7210.h
 * @brief BOX3 板载 ES7210 四通道麦克风 ADC 的公开配置接口。
 *
 * ES7210 由 I2C0 配置，是共享音频总线上真正的四通道 TDM 器件。它工作
 * 在 I2S 从机模式；ESP32-S3 I2S0 提供 MCLK、BCLK 和 WS，并从 DIN 接收
 * 16 bit 1xFS TDM 帧 [MIC1,MIC3,MIC2,MIC4]。MIC1 是环境麦克风，MIC3
 * 是 ES8311 播放参考，MIC2/MIC4 接地。上层把 MIC1+MIC2 保存为双声道
 * WAV（右声道预期接近静音），并可另取 MIC1+MIC3 做 AEC。推荐调用顺序：
 *
 *   board_i2c_init()
 *       -> audio_i2s_init()
 *       -> es7210_init()
 *       -> audio_i2s_record_start()
 *
 * 初始化和重新配置 Codec 时，I2S TX 必须持续输出与目标配置匹配的时钟。
 * es7210_config_codec() 会复位 ADC，必须在录音 RX 停止后调用。
 *
 * 寄存器地址、位掩码、时钟表和 I2C 重试逻辑均属于驱动内部实现，只放在
 * es7210.c 中；应用层应通过本头文件的类型和函数操作 Codec。
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ES7210 串行音频数据格式。
 *
 * 枚举值对应寄存器 0x11 的格式位，不能直接作为 ESP-IDF I2S 枚举使用。
 */
typedef enum {
    ES7210_I2S_FMT_I2S = 0x00,            /**< Philips I2S。 */
    ES7210_I2S_FMT_LEFT_JUSTIFIED = 0x01, /**< 左对齐格式。 */
    ES7210_I2S_FMT_DSP_A = 0x03,          /**< DSP/PCM A 格式。 */
    ES7210_I2S_FMT_DSP_B = 0x13,          /**< DSP/PCM B 格式。 */
} es7210_i2s_format_t;

/** ES7210 串行接口中每个通道采样点的有效数据位宽。 */
typedef enum {
    ES7210_I2S_BITS_16 = 16, /**< 16 bit。 */
    ES7210_I2S_BITS_18 = 18, /**< 18 bit。 */
    ES7210_I2S_BITS_20 = 20, /**< 20 bit。 */
    ES7210_I2S_BITS_24 = 24, /**< 24 bit。 */
    ES7210_I2S_BITS_32 = 32, /**< 32 bit。 */
} es7210_i2s_bits_t;

/**
 * @brief 四路麦克风 PGA 的模拟增益。
 *
 * 枚举值就是增益寄存器的低四位编码。所有通道使用同一个配置值；模拟增益
 * 过大会在 ADC 转换前削顶，正常语音建议从 24 dB 开始调整。
 */
typedef enum {
    ES7210_MIC_GAIN_0_DB = 0x00,
    ES7210_MIC_GAIN_3_DB = 0x01,
    ES7210_MIC_GAIN_6_DB = 0x02,
    ES7210_MIC_GAIN_9_DB = 0x03,
    ES7210_MIC_GAIN_12_DB = 0x04,
    ES7210_MIC_GAIN_15_DB = 0x05,
    ES7210_MIC_GAIN_18_DB = 0x06,
    ES7210_MIC_GAIN_21_DB = 0x07,
    ES7210_MIC_GAIN_24_DB = 0x08,
    ES7210_MIC_GAIN_27_DB = 0x09,
    ES7210_MIC_GAIN_30_DB = 0x0A,
    ES7210_MIC_GAIN_33_DB = 0x0B,
    ES7210_MIC_GAIN_34_5_DB = 0x0C,
    ES7210_MIC_GAIN_36_DB = 0x0D,
    ES7210_MIC_GAIN_37_5_DB = 0x0E,
} es7210_mic_gain_t;

/**
 * @brief MICBIAS 输出电压。
 *
 * 驱动会把同一个电压同时写入 MIC1/2 和 MIC3/4 两组偏置寄存器。
 */
typedef enum {
    ES7210_MIC_BIAS_2V18 = 0x00, /**< 2.18 V。 */
    ES7210_MIC_BIAS_2V26 = 0x10, /**< 2.26 V。 */
    ES7210_MIC_BIAS_2V36 = 0x20, /**< 2.36 V。 */
    ES7210_MIC_BIAS_2V45 = 0x30, /**< 2.45 V。 */
    ES7210_MIC_BIAS_2V55 = 0x40, /**< 2.55 V。 */
    ES7210_MIC_BIAS_2V66 = 0x50, /**< 2.66 V。 */
    ES7210_MIC_BIAS_2V78 = 0x60, /**< 2.78 V。 */
    ES7210_MIC_BIAS_2V87 = 0x70, /**< 2.87 V。 */
} es7210_mic_bias_t;

/**
 * @brief ES7210 时钟、串行接口及模拟输入配置。
 *
 * MCLK 频率按 sample_rate_hz x mclk_ratio 计算，该结果必须能在驱动内部
 * 时钟表中精确匹配。当前时钟表覆盖正点原子参考驱动的 8～96 kHz 组合。
 */
typedef struct {
    uint32_t sample_rate_hz;       /**< WS/LRCK 采样率，单位 Hz。 */
    uint32_t mclk_ratio;           /**< MCLK 与采样率的整数倍频比。 */
    es7210_i2s_format_t i2s_format; /**< 串行音频数据格式。 */
    es7210_i2s_bits_t bit_width;   /**< 每个通道的有效采样位数。 */
    es7210_mic_bias_t mic_bias;    /**< 两组麦克风的偏置电压。 */
    es7210_mic_gain_t mic_gain;    /**< 四路 PGA 使用的模拟增益。 */
    bool tdm_enabled;              /**< true 从 SDOUT1 输出四路 ADC TDM 帧。 */
} es7210_codec_config_t;

/**
 * @brief 初始化 BOX3 板载 ES7210。
 *
 * 完整执行固定地址探测、添加 I2C 设备、八阶段 Codec 配置、关键寄存器
 * 回读以及数字音量设置。默认参数为：
 *
 * - 48 kHz、256fs MCLK；
 * - 16 bit、1xFS 四槽 Philips I2S-TDM，槽顺序 MIC1/MIC3/MIC2/MIC4；
 * - 2.87 V MICBIAS、24 dB PGA 模拟增益；
 * - 0 dB ADC 数字增益，为语音峰值保留削顶余量。
 *
 * 函数可重复调用，初始化成功后再次调用会直接返回。首次调用前必须已经
 * 初始化板级 I2C，并让 I2S 输出稳定的 MCLK/BCLK/WS。
 *
 * @return ESP_OK 初始化成功；否则返回 I2C 探测、设备添加、参数、时钟表、
 *         寄存器传输或回读校验错误。
 */
esp_err_t es7210_init(void);

/**
 * @brief 重新配置 ES7210 的完整录音信号路径。
 *
 * 调用前器件必须已由 es7210_init() 挂接到 I2C 总线。函数先验证全部参数，
 * 再执行软件复位、串行接口、模拟输入、时钟、电源和运行状态配置，最后回读
 * 关键寄存器。应在 I2S RX 停止且目标时钟已经稳定时调用。
 *
 * 本函数不会显式设置 ADC 数字音量，但配置过程包含软件复位；需要固定数字
 * 增益时，应在重新配置成功后再次调用 es7210_config_volume()。配置中途
 * 失败会把驱动恢复为“未初始化”状态，可再次调用 es7210_init() 按默认
 * 参数重新建立完整配置。
 *
 * @param config 有效的配置结构体指针，函数调用期间必须保持可读。
 * @return ESP_OK 配置并校验成功；ESP_ERR_INVALID_STATE 尚未初始化；
 *         ESP_ERR_INVALID_ARG 指针、数值或枚举无效；
 *         ESP_ERR_NOT_SUPPORTED 时钟组合不受支持；其他值为 I2C/校验错误。
 */
esp_err_t es7210_config_codec(const es7210_codec_config_t *config);

/**
 * @brief 设置四路 ADC 相同的数字音量。
 *
 * ES7210 寄存器步进为 0.5 dB，但当前接口使用整数 dB：-95 dB 对应寄存器
 * 0x01，0 dB 对应 0xBF，+32 dB 对应 0xFF。驱动会设置并回读校验四个 ADC
 * 通道。正数字增益会降低削顶余量，默认保持 0 dB。
 *
 * @param volume_db 四路 ADC 的整数数字增益，有效范围 -95～+32 dB。
 * @return ESP_OK 设置并校验成功；ESP_ERR_INVALID_STATE 尚未初始化；
 *         ESP_ERR_INVALID_ARG 超出范围；其他值为 I2C/校验错误。
 */
esp_err_t es7210_config_volume(int8_t volume_db);

#ifdef __cplusplus
}
#endif

#endif
