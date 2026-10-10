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
    char file_name[AUDIO_RECORDER_FILE_NAME_MAX];
} audio_recorder_status_t;

/**
 * AEC 输入回调。
 *
 * mic1_samples 是近端麦克风，mic3_samples 是板上从 ES8311 输出取得的播放
 * 参考；两组数据在同一个 TDM 帧中采样，长度均为 sample_count。指针只在
 * 回调期间有效。回调在录音任务中执行，不应阻塞或直接操作 LVGL/SD 卡。
 */
typedef void (*audio_recorder_aec_input_callback_t)(
    const int16_t *mic1_samples,
    const int16_t *mic3_samples,
    size_t sample_count,
    void *user_context);

/** 初始化 ES7210、录音缓存、消息队列及后台任务。 */
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
 * 录音文件仍固定保存 MIC1+MIC2。注册回调后，每次收到四槽 TDM 数据还会
 * 独立提取 MIC1+MIC3 并调用它，供后续 AEC 算法消费。
 */
esp_err_t audio_recorder_set_aec_input_callback(
    audio_recorder_aec_input_callback_t callback,
    void *user_context);

#endif
