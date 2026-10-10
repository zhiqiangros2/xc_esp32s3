#ifndef BSP_AUDIO_RECORDER_H
#define BSP_AUDIO_RECORDER_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define AUDIO_RECORDER_FILE_NAME_MAX 32

typedef enum {
    AUDIO_RECORDER_IDLE,
    AUDIO_RECORDER_STARTING,
    AUDIO_RECORDER_RECORDING,
    AUDIO_RECORDER_STOPPING,
    AUDIO_RECORDER_SAVED,
    AUDIO_RECORDER_ERROR,
} audio_recorder_state_t;

typedef struct {
    audio_recorder_state_t state;
    esp_err_t last_error;
    uint32_t elapsed_seconds;
    /* 仅当 state=AUDIO_RECORDER_SAVED 时包含最终保存的文件名，其余状态为空。 */
    char file_name[AUDIO_RECORDER_FILE_NAME_MAX];
} audio_recorder_status_t;

/**
 * AEC 输入回调。
 *
 * mic1_samples 是近端麦克风，mic3_samples 是板上从 ES8311 输出取得的播放
 * 参考。两者都是未经 AFE 处理的 16 kHz、16 bit 平面数组，来自同一批
 * TDM 帧且长度均为 sample_count，所以下标相同的两个采样在时间上对齐。
 * 指针只在回调期间有效。回调在常驻 RX 任务中执行，只应快速复制数据，不能
 * 阻塞，也不能直接执行 LVGL、SD 卡或语音模型处理。
 */
typedef void (*audio_recorder_aec_input_callback_t)(
    const int16_t *mic1_samples,
    const int16_t *mic3_samples,
    size_t sample_count,
    void *user_context);

/**
 * 初始化录音缓存、命令队列、后台任务，并启动 16 kHz 常驻 I2S RX。
 *
 * 本函数不初始化 ES7210。调用前必须已经依次完成 audio_i2s_init() 和
 * es7210_init()，确保公共时钟与四槽 TDM 录音硬件已经配置完成。初始化后
 * 即使没有创建 WAV 文件也会持续采集。每个四槽帧同时形成两条逻辑通路：
 * MIC1+MIC3 始终提供给 AEC/语音识别；MIC1+MIC2 仅在录音会话期间写入 WAV。
 * audio_recorder_start()/stop() 不会启动或停止 I2S RX。
 */
esp_err_t audio_recorder_init(void);

/** 异步请求开始一个新的 WAV 文件；函数本身不访问 SD 卡。 */
esp_err_t audio_recorder_start(void);

/** 异步请求停止录音并回填 WAV 文件头。 */
esp_err_t audio_recorder_stop(void);

/** 取得可供 LVGL 定时刷新使用的录音状态快照。 */
esp_err_t audio_recorder_get_status(audio_recorder_status_t *status);

/**
 * 设置 AEC 数据接收回调；传入 NULL 可取消。
 *
 * 录音文件固定保存原始 16 kHz、16 bit 的 MIC1+MIC2。注册回调后，每次
 * 收到四槽 TDM 数据都会独立提取 MIC1+MIC3 并调用它，是否正在保存 WAV
 * 不影响回调；回调后的 ESP-SR 结果也不会写回 WAV。
 */
esp_err_t audio_recorder_set_aec_input_callback(
    audio_recorder_aec_input_callback_t callback,
    void *user_context);

#endif
