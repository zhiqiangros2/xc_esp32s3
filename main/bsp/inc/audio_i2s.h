#ifndef BOARD_AUDIO_I2S_H
#define BOARD_AUDIO_I2S_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * 初始化 BOX3 的 16 位、四槽 Philips TDM 全双工通道。
 *
 * ESP32-S3 产生共享的 MCLK/BCLK/WS；TX 经独立数据线连接 ES8311，RX 经
 * 独立数据线连接 ES7210。四槽配置由 ES7210 的四通道采集需求决定，TX 与
 * RX 为保持同一全双工帧时序而使用相同槽数。
 */
esp_err_t audio_i2s_init(void);

/**
 * 登记一次播放并按需设置公共 I2S 采样率。
 *
 * 录音已经运行时，只允许启动相同采样率的播放，因为 TX/RX 共用 MCLK、
 * BCLK 和 WS。
 */
esp_err_t audio_i2s_playback_start(uint32_t sample_rate_hz);

/** 结束当前播放登记；TX 继续输出静音和 Codec 时钟。 */
esp_err_t audio_i2s_playback_stop(void);

/**
 * 设置录音采样率并启动 I2S 接收通道。
 *
 * 播放已经运行时，只允许启动相同采样率的录音。
 */
esp_err_t audio_i2s_record_start(uint32_t sample_rate_hz);

/** 停止 I2S 接收通道；发送通道继续输出时钟。 */
esp_err_t audio_i2s_record_stop(void);

/**
 * 把 16 位交错双声道 PCM 写入播放通道。
 *
 * 驱动内部把每帧 [L,R] 展开为 [slot0=L, slot1=0, slot2=R, slot3=0]。
 * slot0/2 分别是 LRCK 低/高半周期中 ES8311 能读取的 16 bit 数据，slot1/3
 * 是填充槽。不能写成 [L,R,0,0]，否则 R 会落入左声道窗口的填充槽。
 *
 * ES8311 只有一个 DAC，当前配置选择 slot0 的左声道；OUTP/OUTN 是该单声道
 * 的差分输出，不是左右声道。本接口只保持输入 PCM 的 I2S 位置，不做下混。
 * bytes_written 返回已消耗的输入 PCM 字节数，不包含填充槽；data_size 必须
 * 是一个 [L,R] 帧（4 字节）的整数倍。
 */
esp_err_t audio_i2s_write(const void *data,
                          size_t data_size,
                          size_t *bytes_written,
                          uint32_t timeout_ms);

/**
 * 从 I2S DMA 队列读取 16 位四槽 TDM 原始数据。
 *
 * ES7210 单芯片 1xFS I2S-TDM 的 DMA 帧顺序为
 * [slot0=MIC1, slot1=MIC3, slot2=MIC2, slot3=MIC4]。每槽为 int16_t，
 * 所以返回字节数必为 8 字节帧的整数倍。WAV/AEC 所需通道由上层从这四槽
 * 原始数据中选择，本接口不重排也不丢弃通道。
 */
esp_err_t audio_i2s_read(void *data,
                         size_t data_size,
                         size_t *bytes_read,
                         uint32_t timeout_ms);

#endif
