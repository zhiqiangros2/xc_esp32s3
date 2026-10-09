#ifndef BOARD_AUDIO_I2S_H
#define BOARD_AUDIO_I2S_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** 初始化一次 BOX3 播放通道，固定使用 16 位双声道 Philips I2S。 */
esp_err_t audio_i2s_init(uint32_t sample_rate_hz);

/** 在通道停止后切换采样率，并重新启动发送通道。 */
esp_err_t audio_i2s_set_sample_rate(uint32_t sample_rate_hz);

/** 把 PCM 数据写入 I2S DMA 队列。 */
esp_err_t audio_i2s_write(const void *data,
                          size_t data_size,
                          size_t *bytes_written,
                          uint32_t timeout_ms);

#endif
