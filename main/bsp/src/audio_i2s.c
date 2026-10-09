/**
 * @file audio_i2s.c
 * @brief ESP32-S3 到 ES8311 的 I2S 发送通道封装。
 *
 * 本文件只负责数字 PCM 数据链路：ESP32-S3 作为 I2S 主机产生 MCLK、BCLK、
 * WS，并通过 DOUT 发送 16 bit 双声道 PCM。ES8311 的寄存器初始化、音量和
 * 静音由 es8311.c 负责，板级功放使能由 AW9523B 驱动负责。
 *
 * 调用顺序：
 * 1. audio_i2s_init() 创建并启动发送通道，使 ES8311 先获得稳定时钟；
 * 2. 初始化 ES8311；
 * 3. 使用 audio_i2s_write() 持续向 DMA 队列提交 PCM 数据。
 */
#include "audio_i2s.h"

#include <inttypes.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

/* BOX3 原理图固定使用 I2S0 连接板载 ES8311。 */
#define AUDIO_I2S_PORT I2S_NUM_0
/* 主时钟 MCLK：供 ES8311 内部时钟电路使用。 */
#define AUDIO_I2S_MCLK_GPIO GPIO_NUM_21
/* 位时钟 BCLK：每传输一个 PCM 数据位跳变一次。 */
#define AUDIO_I2S_BCLK_GPIO GPIO_NUM_38
/* 左右声道选择时钟 WS/LRCK：频率等于音频采样率。 */
#define AUDIO_I2S_WS_GPIO GPIO_NUM_39
/* ESP32-S3 数据输出，连接 ES8311 的串行音频输入。 */
#define AUDIO_I2S_DOUT_GPIO GPIO_NUM_40
/*
 * dma_frame_num 的“一帧”包含一次左声道采样和一次右声道采样：
 *   单声道采样大小 = 16 bit / 8 = 2 字节；
 *   一帧大小       = 2 字节 x 2 声道 = 4 字节；
 *   单个 DMA 数据缓冲区 = 511 帧 x 4 字节 = 2044 字节；
 *   12 个缓冲区总大小   = 2044 字节 x 12 = 24528 字节。
 *
 * 这里是“每个 DMA 描述符关联一个 2044 字节的数据缓冲区”，并不是描述符
 * 结构本身占用 2044 字节。48 kHz 音频每秒的数据量为
 * 48000 帧 x 4 字节 = 192000 字节，所以全部 DMA 缓冲可维持：
 *   24528 / 192000 = 0.12775 秒，约 128 ms。
 *
 * 这段时间用于吸收 SD 卡读取、SPI 总线占用和界面刷新造成的短时调度抖动。
 * 511 帧还可确保单个 DMA 数据块不超过 ESP-IDF 要求的 4092 字节上限。
 */
#define AUDIO_I2S_DMA_DESCRIPTOR_COUNT 12
#define AUDIO_I2S_DMA_FRAME_COUNT 511

/* 非 NULL 表示 I2S 通道已经创建；句柄由 ESP-IDF I2S 驱动拥有。 */
static i2s_chan_handle_t s_tx_channel;
/* 区分“通道已创建”和“通道正在运行”，用于失败后的再次启用。 */
static bool s_tx_channel_enabled;
/* 记录当前硬件采样率，避免重复停止并重配时钟。 */
static uint32_t s_sample_rate_hz;
static const char *TAG = "AUDIO_I2S";

/**
 * @brief 创建并启动 I2S0 标准模式发送通道。
 *
 * 配置固定为 Philips I2S、16 bit、双声道、ESP32-S3 主机模式。本函数只在
 * main.c 的板级启动流程中调用一次；播放不同采样率文件时使用
 * audio_i2s_set_sample_rate()，不重复创建硬件通道。
 *
 * @param sample_rate_hz WAV 文件的采样率，必须大于 0。
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；其他值来自 I2S 驱动。
 */
esp_err_t audio_i2s_init(uint32_t sample_rate_hz)
{
    /* 0 Hz 无法生成有效的 WS 和位时钟。 */
    if (sample_rate_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * 只创建 TX 通道。auto_clear 会在发送下溢时自动输出 0，避免重复播放
     * DMA 中的旧采样；听感上可能是短暂静音，而不是刺耳的循环噪声。
     */
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = AUDIO_I2S_DMA_DESCRIPTOR_COUNT;
    channel_config.dma_frame_num = AUDIO_I2S_DMA_FRAME_COUNT;
    channel_config.auto_clear = true;

    esp_err_t result = i2s_new_channel(&channel_config,
                                       &s_tx_channel,
                                       NULL);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "i2s_new_channel(I2S0 TX) failed: %s",
                 esp_err_to_name(result));
        s_tx_channel = NULL;
        return result;
    }
    s_tx_channel_enabled = false;

    /*
     * Philips 标准模式中 WS 在下一个声道最高有效位之前翻转。din 未使用，
     * 因为当前功能只播放，不从 ES8311 录音。所有时钟均保持板级正常极性。
     */
    const i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK_GPIO,
            .bclk = AUDIO_I2S_BCLK_GPIO,
            .ws = AUDIO_I2S_WS_GPIO,
            .dout = AUDIO_I2S_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    /* 先把新通道配置成标准模式，再启用 DMA 和时钟输出。 */
    result = i2s_channel_init_std_mode(s_tx_channel, &standard_config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "i2s_channel_init_std_mode failed: %s",
                 esp_err_to_name(result));
    }
    if (result == ESP_OK) {
        result = i2s_channel_enable(s_tx_channel);
        if (result != ESP_OK) {
            ESP_LOGE(TAG,
                     "i2s_channel_enable failed: %s",
                     esp_err_to_name(result));
        }
    }
    /* 任一阶段失败都删除已创建通道，下一次调用可以从干净状态重试。 */
    if (result != ESP_OK) {
        i2s_del_channel(s_tx_channel);
        s_tx_channel = NULL;
        s_tx_channel_enabled = false;
        return result;
    }

    s_sample_rate_hz = sample_rate_hz;
    s_tx_channel_enabled = true;
    ESP_LOGI(TAG,
             "I2S0 TX ready: %" PRIu32 " Hz, 16-bit stereo",
             sample_rate_hz);
    return ESP_OK;
}

/**
 * @brief 在保留 DMA 通道的情况下修改 I2S 采样率。
 *
 * ESP-IDF 要求标准模式时钟只能在通道停止时重配。采样率未变化且通道正在
 * 运行时直接返回；通道曾启动失败时只重试启用；采样率变化时执行“停用、
 * 重配、重新启用”。重配失败仍尝试恢复旧配置对应的通道运行状态。
 *
 * @param sample_rate_hz 新采样率，必须大于 0。
 * @return ESP_OK 成功；否则返回参数检查或 I2S 驱动错误。
 */
esp_err_t audio_i2s_set_sample_rate(uint32_t sample_rate_hz)
{
    if (sample_rate_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tx_channel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const bool sample_rate_changed = sample_rate_hz != s_sample_rate_hz;
    if (!sample_rate_changed && s_tx_channel_enabled) {
        return ESP_OK;
    }

    /* 只有正在运行且确实需要重配时钟时才停用通道。 */
    if (sample_rate_changed && s_tx_channel_enabled) {
        const esp_err_t disable_result = i2s_channel_disable(s_tx_channel);
        if (disable_result != ESP_OK) {
            ESP_LOGE(TAG,
                     "Cannot stop I2S before changing sample rate: %s",
                     esp_err_to_name(disable_result));
            return disable_result;
        }
        s_tx_channel_enabled = false;
    }

    esp_err_t reconfigure_result = ESP_OK;
    if (sample_rate_changed) {
        const i2s_std_clk_config_t clock_config =
            I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
        reconfigure_result =
            i2s_channel_reconfig_std_clock(s_tx_channel, &clock_config);
        if (reconfigure_result == ESP_OK) {
            /* 硬件时钟已经采用新配置，即使后续 enable 失败也应记录新采样率。 */
            s_sample_rate_hz = sample_rate_hz;
        } else {
            ESP_LOGE(TAG,
                     "Cannot set I2S sample rate to %" PRIu32 " Hz: %s",
                     sample_rate_hz,
                     esp_err_to_name(reconfigure_result));
        }
    }

    /* 无论重配是否成功都尝试启动通道；失败时下次调用会再次执行 enable。 */
    const esp_err_t enable_result = i2s_channel_enable(s_tx_channel);
    if (enable_result == ESP_OK) {
        s_tx_channel_enabled = true;
    } else {
        s_tx_channel_enabled = false;
        ESP_LOGE(TAG,
                 "Cannot enable I2S TX channel: %s",
                 esp_err_to_name(enable_result));
    }
    return reconfigure_result != ESP_OK ? reconfigure_result : enable_result;
}

/**
 * @brief 把一段交错排列的双声道 PCM 数据复制到 I2S DMA 队列。
 *
 * 本函数可能阻塞到 DMA 腾出空间或 timeout_ms 到期。bytes_written 即使在
 * ESP_ERR_TIMEOUT 时也可能大于 0，调用方必须按实际写入量推进数据指针。
 *
 * @param data PCM 数据首地址，格式必须与初始化配置一致。
 * @param data_size 希望提交的字节数，必须大于 0。
 * @param bytes_written 返回实际进入 DMA 队列的字节数。
 * @param timeout_ms 最长等待时间，单位毫秒。
 * @return ESP_OK 全部写入；ESP_ERR_TIMEOUT 超时；或参数、状态错误。
 */
esp_err_t audio_i2s_write(const void *data,
                          size_t data_size,
                          size_t *bytes_written,
                          uint32_t timeout_ms)
{
    if (s_tx_channel == NULL || !s_tx_channel_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL || data_size == 0 || bytes_written == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * i2s_channel_write() 的参数单位已经是毫秒，驱动内部会自行调用
     * pdMS_TO_TICKS()。这里必须直接传 timeout_ms；若提前转换一次，在
     * CONFIG_FREERTOS_HZ=100 时，100 ms 会被错误缩短成 10 ms。
     */
    return i2s_channel_write(s_tx_channel,
                             data,
                             data_size,
                             bytes_written,
                             timeout_ms);
}
