#ifndef BOARD_AUDIO_I2S_H
#define BOARD_AUDIO_I2S_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * BOX3 播放、录音和语音识别共用同一组 MCLK/BCLK/LRCK，因此整个音频链
 * 固定使用 16 kHz。四槽、16 bit 时：LRCK=16 kHz，BCLK=1.024 MHz；
 * ESP-IDF 默认使用 256fs MCLK，因此 MCLK=4.096 MHz。
 */
#define AUDIO_I2S_SAMPLE_RATE_HZ 16000U

/**
 * 初始化 BOX3 的 16 kHz、16 位、四槽 Philips TDM 全双工通道。
 *
 * ESP32-S3 使用 5 根 GPIO 完成全双工音频连接：
 *
 *   GPIO21 MCLK ----+----> ES8311 MCLK
 *                   +----> ES7210 MCLK
 *   GPIO38 BCLK ----+----> ES8311 SCLK
 *                   +----> ES7210 SCLK
 *   GPIO39 LRCK ----+----> ES8311 LRCK
 *                   +----> ES7210 LRCK
 *   GPIO40 DOUT ----------> ES8311 DSDIN              播放数据
 *   GPIO41 DIN  <---------- ES7210 SDOUT1/TDMOUT      录音数据
 *
 * MCLK/BCLK/LRCK 是两个 Codec 共用的三根时钟线；GPIO40 和 GPIO41 是方向
 * 相反、彼此独立的两根数据线，所以 TX 播放与 RX 录音能够同时执行。四槽
 * 配置由 ES7210 的四通道采集需求决定，TX/RX 必须保持相同的时钟和槽边界。
 * I2C SDA/SCL 只用于 Codec 寄存器配置，不属于上述 PCM 音频线。
 */
esp_err_t audio_i2s_init(void);

/**
 * 登记一次 16 kHz 播放。
 *
 * sample_rate_hz 必须等于 AUDIO_I2S_SAMPLE_RATE_HZ。TX/RX 共用 MCLK、
 * BCLK 和 WS，运行期间不允许播放文件改变常驻录音和识别所用的采样率。
 */
esp_err_t audio_i2s_playback_start(uint32_t sample_rate_hz);

/** 结束当前播放登记；TX 继续输出静音和 Codec 时钟。 */
esp_err_t audio_i2s_playback_stop(void);

/**
 * 启动固定 16 kHz 的 I2S 接收通道。
 *
 * sample_rate_hz 必须等于 AUDIO_I2S_SAMPLE_RATE_HZ。
 */
esp_err_t audio_i2s_record_start(uint32_t sample_rate_hz);

/** 停止 I2S 接收通道；发送通道继续输出时钟。 */
esp_err_t audio_i2s_record_stop(void);

/**
 * 把 16 位交错双声道 PCM 写入播放通道。
 *
 * 驱动内部把每帧 [L,R] 展开为 [slot0=L, slot1=0, slot2=R, slot3=0]。
 * 一个 LRCK 周期有 64 BCLK：低、高半周期各占两个 16 bit 槽。ES8311 当前
 * 配置为 16 bit I2S，所以只读取低半周期开头的 slot0 和高半周期开头的
 * slot2；slot1/3 是填充时间，不是额外声道。不能写成 [L,R,0,0]，否则 R
 * 仍在 LRCK 低电平期间发送，会被当作左窗口填充，而右窗口实际读到 slot2=0。
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
