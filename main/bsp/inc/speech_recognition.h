#ifndef BSP_SPEECH_RECOGNITION_H
#define BSP_SPEECH_RECOGNITION_H

#include "esp_err.h"

/**
 * 初始化常驻语音识别链路。
 *
 * 调用前必须完成 aw9523b_init()、es7210_init() 和 audio_recorder_init()。
 * 模块从 audio_recorder 的常驻回调取得原生 16 kHz、16 bit MIC1+MIC3：
 * MIC1 是近端麦克风 M，MIC3 是 ES8311 播放参考 R。I2S 采集率与 AFE 要求
 * 已经一致，因此直接以 [M,R] 交错格式送入 ESP-SR，不做 FIR 或降采样。
 *
 * AFE 启用 AEC、NS、VAD、AGC 和 WakeNet，输出 16 kHz 单声道增强语音及
 * 唤醒状态；检测到“你好小鑫”后才把 AFE 输出送给 MultiNet。支持
 * “打开红灯”和“关闭红灯”两条命令。
 *
 * 该识别通路不改变 WAV 数据。WAV 仍由 audio_recorder 独立保存未经 AFE
 * 处理的 16 kHz、16 bit、双声道 [MIC1,MIC2]。
 */
esp_err_t speech_recognition_init(void);

#endif
