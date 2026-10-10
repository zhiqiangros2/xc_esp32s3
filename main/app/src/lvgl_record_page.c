/**
 * @file lvgl_record_page.c
 * @brief ES7210 录音控制及状态页面。
 *
 * 所有控件只由 LVGL 任务访问。按钮向 audio_recorder 后台任务发送事件，
 * 200 ms 定时器读取状态快照；页面不执行 SD 或 I2S 阻塞操作。
 */
#include "lvgl_record_page.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_recorder.h"

#define RECORD_STATUS_UPDATE_MS 200U

typedef struct {
    lv_obj_t *page;
    lv_obj_t *return_page;
    lv_obj_t *status_label;
    lv_obj_t *time_label;
    lv_obj_t *file_label;
    lv_obj_t *start_button;
    lv_obj_t *stop_button;
    lv_timer_t *status_timer;
    lvgl_language_t language;
    const lv_font_t *ui_font;
    bool closing;
    /* false 表示本次进入页面后尚未开始录音，不显示上一次会话的数据。 */
    bool session_started;
    audio_recorder_state_t displayed_state;
    uint32_t displayed_seconds;
    char displayed_file[AUDIO_RECORDER_FILE_NAME_MAX];
} record_page_context_t;

static const lv_font_t *interface_font(const record_page_context_t *context)
{
    if (context->language == LVGL_LANGUAGE_ZH_CN &&
        context->ui_font != NULL) {
        return context->ui_font;
    }
    return LV_FONT_DEFAULT;
}

static bool state_is_active(audio_recorder_state_t state)
{
    return state == AUDIO_RECORDER_STARTING ||
           state == AUDIO_RECORDER_RECORDING ||
           state == AUDIO_RECORDER_STOPPING;
}

/** 返回原页面并安排删除录音页面；同时停止定时器，避免重复安排删除。 */
static void leave_record_page(record_page_context_t *context)
{
    if (context->status_timer != NULL) {
        lv_timer_delete(context->status_timer);
        context->status_timer = NULL;
    }
    lv_screen_load(context->return_page);
    lv_obj_delete_async(context->page);
}

static void update_page(record_page_context_t *context)
{
    audio_recorder_status_t status;
    if (audio_recorder_get_status(&status) != ESP_OK) {
        return;
    }

    /* 关闭请求会反复确认 STOP 已入队；只有录音彻底结束后才销毁页面。 */
    if (context->closing) {
        if (status.state == AUDIO_RECORDER_STARTING ||
            status.state == AUDIO_RECORDER_RECORDING) {
            (void)audio_recorder_stop();
        } else if (!state_is_active(status.state)) {
            leave_record_page(context);
            return;
        }
    }

    /*
     * audio_recorder 会保留上一段录音的 SAVED 状态和最终秒数，供其他调用者
     * 查询。新建页面时不应把这段旧时长带进来，所以在本页面第一次成功发送
     * START 前固定显示 00:00；START 接口会同步把后台秒数清零，此后再显示
     * 本次会话的实时秒数以及停止后的最终时长。
     */
    const uint32_t visible_seconds =
        context->session_started ? status.elapsed_seconds : 0U;
    /*
     * 文件名只在本页面启动的录音成功保存后显示。刚进入页面时，即使后台
     * 仍保留上一次 SAVED 状态，也必须显示为空；开始下一次录音时同样清空。
     */
    const char *visible_file =
        context->session_started && status.state == AUDIO_RECORDER_SAVED
            ? status.file_name
            : "";

    if (status.state == context->displayed_state &&
        visible_seconds == context->displayed_seconds &&
        strcmp(visible_file, context->displayed_file) == 0) {
        return;
    }

    const lvgl_language_texts_t *texts =
        lvgl_language_get_texts(context->language);
    const char *state_text = texts->record_idle;
    switch (status.state) {
        case AUDIO_RECORDER_STARTING:
            state_text = texts->record_starting;
            break;
        case AUDIO_RECORDER_RECORDING:
            state_text = texts->record_recording;
            break;
        case AUDIO_RECORDER_STOPPING:
            state_text = texts->record_stopping;
            break;
        case AUDIO_RECORDER_SAVED:
            state_text = texts->record_saved;
            break;
        case AUDIO_RECORDER_ERROR:
            state_text = texts->record_error;
            break;
        case AUDIO_RECORDER_IDLE:
        default:
            break;
    }

    lv_label_set_text(context->status_label, state_text);
    lv_label_set_text_fmt(context->time_label,
                          "%02" PRIu32 ":%02" PRIu32,
                          visible_seconds / 60U,
                          visible_seconds % 60U);
    lv_label_set_text(context->file_label, visible_file);

    const bool active = state_is_active(status.state);
    if (active) {
        lv_obj_add_state(context->start_button, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(context->start_button, LV_STATE_DISABLED);
    }
    if (status.state == AUDIO_RECORDER_STARTING ||
        status.state == AUDIO_RECORDER_RECORDING) {
        lv_obj_remove_state(context->stop_button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(context->stop_button, LV_STATE_DISABLED);
    }

    context->displayed_state = status.state;
    context->displayed_seconds = visible_seconds;
    snprintf(context->displayed_file,
             sizeof(context->displayed_file),
             "%s",
             visible_file);
}

static void status_timer_elapsed(lv_timer_t *timer)
{
    record_page_context_t *context = lv_timer_get_user_data(timer);
    update_page(context);
}

static void record_page_deleted(lv_event_t *event)
{
    record_page_context_t *context = lv_event_get_user_data(event);
    if (context->status_timer != NULL) {
        lv_timer_delete(context->status_timer);
        context->status_timer = NULL;
    }
    (void)audio_recorder_stop();
    free(context);
}

static void start_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    record_page_context_t *context = lv_event_get_user_data(event);
    if (audio_recorder_start() == ESP_OK) {
        /*
         * audio_recorder_start() 在命令入队前已把 elapsed_seconds 清为 0。
         * 从这一刻开始显示本次会话时长，并在本事件结束前立即画出 00:00
         * 和 STARTING，不等待下一个 200 ms 定时刷新周期。
         */
        context->session_started = true;
        context->displayed_state = (audio_recorder_state_t)-1;
        update_page(context);
    }
}

static void stop_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    record_page_context_t *context = lv_event_get_user_data(event);
    if (audio_recorder_stop() == ESP_OK) {
        context->displayed_state = (audio_recorder_state_t)-1;
        update_page(context);
    }
}

static void close_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    record_page_context_t *context = lv_event_get_user_data(event);
    audio_recorder_status_t status;
    if (audio_recorder_get_status(&status) != ESP_OK ||
        !state_is_active(status.state)) {
        leave_record_page(context);
        return;
    }

    context->closing = true;
    (void)audio_recorder_stop();
    context->displayed_state = (audio_recorder_state_t)-1;
    update_page(context);
}

lv_obj_t *lvgl_record_page_create(lv_obj_t *return_page,
                                  lvgl_language_t language,
                                  const lv_font_t *ui_font)
{
    if (return_page == NULL) {
        return NULL;
    }

    record_page_context_t *context = calloc(1, sizeof(*context));
    if (context == NULL) {
        return NULL;
    }
    context->return_page = return_page;
    context->language = language;
    context->ui_font = ui_font;
    context->displayed_state = (audio_recorder_state_t)-1;

    lv_obj_t *page = lv_obj_create(NULL);
    if (page == NULL) {
        free(context);
        return NULL;
    }
    context->page = page;
    lv_obj_add_event_cb(page,
                        record_page_deleted,
                        LV_EVENT_DELETE,
                        context);
    lv_obj_set_style_bg_color(page, lv_color_hex(0xF5F7F8), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(page, lv_color_hex(0x17202A), 0);

    const lvgl_language_texts_t *texts = lvgl_language_get_texts(language);
    const lv_font_t *font = interface_font(context);

    lv_obj_t *title = lv_label_create(page);
    lv_label_set_text(title, texts->record);
    lv_obj_set_style_text_font(title, font, 0);
    lv_obj_set_pos(title, 8, 12);

    lv_obj_t *close_button = lv_button_create(page);
    lv_obj_set_size(close_button, 76, 40);
    lv_obj_set_pos(close_button, 244, 2);
    lv_obj_set_style_radius(close_button, 4, 0);
    lv_obj_set_style_bg_color(close_button, lv_color_hex(0x455A64), 0);
    lv_obj_add_event_cb(close_button,
                        close_button_clicked,
                        LV_EVENT_CLICKED,
                        context);
    lv_obj_t *close_label = lv_label_create(close_button);
    lv_label_set_text(close_label, texts->close);
    lv_obj_set_style_text_font(close_label, font, 0);
    lv_obj_center(close_label);

    context->status_label = lv_label_create(page);
    lv_obj_set_size(context->status_label, 304, 28);
    lv_obj_set_pos(context->status_label, 8, 54);
    lv_obj_set_style_text_font(context->status_label, font, 0);
    lv_obj_set_style_text_align(context->status_label, LV_TEXT_ALIGN_CENTER, 0);

    context->time_label = lv_label_create(page);
    lv_obj_set_size(context->time_label, 304, 32);
    lv_obj_set_pos(context->time_label, 8, 86);
    lv_obj_set_style_text_font(context->time_label, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_align(context->time_label, LV_TEXT_ALIGN_CENTER, 0);

    context->file_label = lv_label_create(page);
    lv_obj_set_size(context->file_label, 304, 24);
    lv_obj_set_pos(context->file_label, 8, 126);
    lv_label_set_long_mode(context->file_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(context->file_label, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_align(context->file_label, LV_TEXT_ALIGN_CENTER, 0);

    context->start_button = lv_button_create(page);
    lv_obj_set_size(context->start_button, 136, 52);
    lv_obj_set_pos(context->start_button, 16, 174);
    lv_obj_set_style_radius(context->start_button, 4, 0);
    lv_obj_set_style_bg_color(context->start_button,
                              lv_color_hex(0xC62828),
                              0);
    lv_obj_add_event_cb(context->start_button,
                        start_button_clicked,
                        LV_EVENT_CLICKED,
                        context);
    lv_obj_t *start_label = lv_label_create(context->start_button);
    lv_label_set_text(start_label, texts->record_start);
    lv_obj_set_style_text_font(start_label, font, 0);
    lv_obj_center(start_label);

    context->stop_button = lv_button_create(page);
    lv_obj_set_size(context->stop_button, 136, 52);
    lv_obj_set_pos(context->stop_button, 168, 174);
    lv_obj_set_style_radius(context->stop_button, 4, 0);
    lv_obj_set_style_bg_color(context->stop_button,
                              lv_color_hex(0x37474F),
                              0);
    lv_obj_add_event_cb(context->stop_button,
                        stop_button_clicked,
                        LV_EVENT_CLICKED,
                        context);
    lv_obj_t *stop_label = lv_label_create(context->stop_button);
    lv_label_set_text(stop_label, texts->stop);
    lv_obj_set_style_text_font(stop_label, font, 0);
    lv_obj_center(stop_label);

    update_page(context);
    context->status_timer = lv_timer_create(status_timer_elapsed,
                                             RECORD_STATUS_UPDATE_MS,
                                             context);
    if (context->status_timer == NULL) {
        lv_obj_delete(page);
        return NULL;
    }
    return page;
}
