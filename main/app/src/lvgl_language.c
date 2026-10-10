#include "lvgl_language.h"

static lvgl_language_t s_language = LVGL_LANGUAGE_ZH_CN;

static const lvgl_language_texts_t s_texts[LVGL_LANGUAGE_COUNT] = {
    [LVGL_LANGUAGE_EN] = {
        .test_item = "Test item",
        .back = "Back",
        .music = "Music",
        .stop = "Stop",
        .close = "Close",
        .music_stopped = "Stopped",
        .music_loading = "Loading",
        .music_playing = "Playing",
        .music_error = "Playback failed",
        .music_directory_error = "Cannot open /sdcard/music",
        .music_empty = "No WAV files",
        .record = "Recorder",
        .record_start = "Record",
        .record_idle = "Ready",
        .record_starting = "Starting",
        .record_recording = "Recording",
        .record_stopping = "Saving",
        .record_saved = "Saved",
        .record_error = "Recording failed",
    },
    [LVGL_LANGUAGE_ZH_CN] = {
        .test_item = "测试项",
        .back = "返回",
        .music = "音乐",
        .stop = "停止",
        .close = "关闭",
        .music_stopped = "已停止",
        .music_loading = "正在加载",
        .music_playing = "正在播放",
        .music_error = "播放失败",
        .music_directory_error = "无法打开 /sdcard/music",
        .music_empty = "没有 WAV 文件",
        .record = "录音",
        .record_start = "录音",
        .record_idle = "准备就绪",
        .record_starting = "正在启动",
        .record_recording = "正在录音",
        .record_stopping = "正在保存",
        .record_saved = "已保存",
        .record_error = "录音失败",
    },
};

const lvgl_language_texts_t *lvgl_language_get_texts(lvgl_language_t language)
{
    if (language < 0 || language >= LVGL_LANGUAGE_COUNT) {
        language = LVGL_LANGUAGE_EN;
    }

    return &s_texts[language];
}

esp_err_t lvgl_language_set(lvgl_language_t language)
{
    if (language < 0 || language >= LVGL_LANGUAGE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    s_language = language;
    return ESP_OK;
}

lvgl_language_t lvgl_language_get(void)
{
    return s_language;
}
