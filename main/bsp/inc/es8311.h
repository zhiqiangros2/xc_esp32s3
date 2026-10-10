#ifndef BOARD_ES8311_H
#define BOARD_ES8311_H

/**
 * @file es8311.h
 * @brief BOX3 板载 ES8311 播放 Codec 的公开控制接口。
 *
 * 数字音频连接为三根共享时钟输入加一根播放数据输入：GPIO21/MCLK、
 * GPIO38/BCLK、GPIO39/LRCK 由 ESP32-S3 同时送给 ES8311 和 ES7210；播放
 * PCM 单独经 GPIO40/DOUT 送入 ES8311 DSDIN。GPIO41/DIN 接收的是 ES7210
 * TDM 数据，不是 ES8311 的 ADC 输出。
 *
 * ES8311 固定使用 7 bit I2C 地址 0x18。芯片的串行音频口支持 I2S、左对齐
 * 和 DSP/PCM A/B，字长支持 16/18/20/24/32 bit；本驱动固定配置为 16 bit
 * Philips I2S 从机。DSP/PCM 只是另一种串行帧格式，不表示芯片具有四通道
 * TDM DAC。ES8311 只有一个单声道 DAC；OUTP/OUTN 是同一模拟信号的差分
 * 正负端，不是左右两路输出。寄存器 0x09 当前选择 I2S 左声道窗口，芯片
 * 不会自动混合 L/R。
 *
 * ESP32-S3 I2S0 为兼容 ES7210 四通道采集而输出四槽帧：
 * [slot0=L, slot1=0, slot2=R, slot3=0]。LRCK 的每个半周期占两个 16 bit
 * 物理槽，但 ES8311 的有效字长是 16 bit，所以只读取每个半周期的第一个槽，
 * 第二槽必须填 0；当前单 DAC 实际使用左窗口 slot0。ESP32-S3 必须先启动
 * I2S TX，为 Codec 持续提供 MCLK/BCLK/WS，随后才能初始化本驱动。
 * 推荐的板级调用顺序为：
 *
 *   board_i2c_init()
 *       -> audio_i2s_init()
 *       -> es8311_init()
 *       -> aw9523b_set_box3_pa_enabled(true)
 *
 * es8311_init() 成功后默认音量为 80%，DAC 保持解除静音。停止播放只需停止
 * 提交 PCM；是否关闭外部功放不属于本驱动职责。
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @brief 初始化 BOX3 板载 ES8311 播放通路。
 *
 * 函数执行固定地址探测、添加 I2C 设备、基础时钟/复位、I2S 和 DAC 信号
 * 通路配置，最后设置 80% 音量并解除静音。接口可重复调用；首次成功后再次
 * 调用会直接返回，不会复位正在工作的 Codec。
 *
 * @return ESP_OK 初始化成功；否则返回 I2C 探测、设备添加或寄存器事务错误。
 */
esp_err_t es8311_init(void);

/**
 * @brief 设置 ES8311 DAC 数字音量。
 *
 * 0~100 按比例映射到 DAC 音量寄存器 0x00~0xFF。0% 是最低寄存器音量，
 * 不等同于数字静音；需要保证无声时应调用 es8311_set_mute(true)。
 *
 * @param percent 音量百分比，有效范围 0~100。
 * @return ESP_OK 设置成功；ESP_ERR_INVALID_STATE 尚未初始化；
 *         ESP_ERR_INVALID_ARG 参数超出范围；其他值为 I2C 写入错误。
 */
esp_err_t es8311_set_volume(uint8_t percent);

/**
 * @brief 设置或解除 ES8311 DAC 数字静音。
 *
 * 本函数只修改 Codec 内部静音位，不停止 I2S、不改变当前音量，也不控制
 * AW9523B 管理的外部功放。
 *
 * @param muted true 静音；false 解除静音。
 * @return ESP_OK 设置成功；ESP_ERR_INVALID_STATE 尚未初始化；
 *         其他值为 I2C 读取或写入错误。
 */
esp_err_t es8311_set_mute(bool muted);

#endif
