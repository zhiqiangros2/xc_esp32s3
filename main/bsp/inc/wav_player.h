#ifndef BOARD_WAV_PLAYER_H
#define BOARD_WAV_PLAYER_H

#include <stdbool.h>

#include "esp_err.h"

#define WAV_PLAYER_FILE_NAME_MAX 128

typedef enum {
    WAV_PLAYER_STOPPED,
    WAV_PLAYER_LOADING,
    WAV_PLAYER_PLAYING,
    WAV_PLAYER_ERROR,
} wav_player_state_t;

typedef struct {
    wav_player_state_t state;
    esp_err_t last_error;
    char file_name[WAV_PLAYER_FILE_NAME_MAX];
} wav_player_status_t;

/** 创建 SD 读取任务、DMA 写任务、双缓冲及两个任务收件队列。重复调用安全。 */
esp_err_t wav_player_init(void);

/** 将播放指定 PCM WAV 文件的事件加入队列；播放中取到该事件时会切歌。 */
esp_err_t wav_player_play(const char *path);

/** 将停止事件加入队列；播放器处理该事件后停止，功放保持开启。 */
esp_err_t wav_player_stop(void);

/** 获取播放器状态快照。 */
esp_err_t wav_player_get_status(wav_player_status_t *status);

#endif
