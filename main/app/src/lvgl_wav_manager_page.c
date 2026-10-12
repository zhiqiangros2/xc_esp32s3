/**
 * @file lvgl_wav_manager_page.c
 * @brief /sdcard/music 目录下 WAV 文件的选择和删除页面。
 *
 * 页面先完成固定控件的首帧绘制，再由 LVGL 定时器逐项扫描目录和创建文件按钮，
 * 避免文件较多时阻塞页面切换。删除成功后以相同方式重新扫描；关闭根对象时会
 * 停止扫描、关闭目录，并释放文件按钮和页面上下文。页面及每个文件的路径上下文
 * 都放在 PSRAM，目录文件较多时不会持续占用内部堆。
 */
#include "lvgl_wav_manager_page.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sd_fatfs.h"

#define WAV_MANAGER_DIRECTORY SD_MOUNT_POINT "/music"
#define WAV_MANAGER_PATH_MAX 384U
/* 每次回调只读取一个目录项，让 LVGL 有机会及时处理绘制和触摸事件。 */
#define WAV_MANAGER_SCAN_INTERVAL_MS 5U

typedef struct {
    lv_obj_t *page;
    lv_obj_t *return_page;
    lv_obj_t *list;
    lv_obj_t *status_label;
    lv_obj_t *delete_button;
    lv_obj_t *selected_button;
    /* 增量扫描使用的定时器、目录句柄及“加载中”提示。 */
    lv_timer_t *scan_timer;
    DIR *directory;
    lv_obj_t *scan_label;
    lvgl_language_t language;
    const lv_font_t *ui_font;
    /* 一次扫描的状态；删除后重新扫描时全部复位。 */
    bool scan_started;
    bool initial_scan_requested;
    size_t file_count;
    const char *completed_message;
    char selected_path[WAV_MANAGER_PATH_MAX];
} wav_manager_page_context_t;

typedef struct {
    wav_manager_page_context_t *page_context;
    lv_obj_t *button;
    char path[WAV_MANAGER_PATH_MAX];
} wav_manager_file_context_t;

static const char *TAG = "WAV_MANAGER";

/** 中文界面使用完整字库；英文界面使用 LVGL 默认字体。 */
static const lv_font_t *interface_font(
    const wav_manager_page_context_t *context)
{
    if (context->language == LVGL_LANGUAGE_ZH_CN &&
        context->ui_font != NULL) {
        return context->ui_font;
    }
    return LV_FONT_DEFAULT;
}

/** 仅接受不区分大小写、以 .wav 结尾的文件名。 */
static bool is_wav_file(const char *name)
{
    const char *extension = strrchr(name, '.');
    if (extension == NULL || strlen(extension) != 4U) {
        return false;
    }

    return (extension[1] == 'w' || extension[1] == 'W') &&
           (extension[2] == 'a' || extension[2] == 'A') &&
           (extension[3] == 'v' || extension[3] == 'V') &&
           extension[4] == '\0';
}

/** 清除当前选择，并禁用删除按钮。 */
static void clear_selection(wav_manager_page_context_t *context)
{
    context->selected_button = NULL;
    context->selected_path[0] = '\0';
    lv_obj_add_state(context->delete_button, LV_STATE_DISABLED);
}

/** 文件按钮删除时释放它独占的路径和页面回指上下文。 */
static void file_context_deleted(lv_event_t *event)
{
    heap_caps_free(lv_event_get_user_data(event));
}

/** 选中一个文件；同一时间只有一个按钮保持选中颜色。 */
static void file_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    wav_manager_file_context_t *file = lv_event_get_user_data(event);
    wav_manager_page_context_t *context = file->page_context;
    if (context->selected_button != NULL &&
        context->selected_button != file->button) {
        lv_obj_remove_state(context->selected_button, LV_STATE_CHECKED);
    }

    context->selected_button = file->button;
    snprintf(context->selected_path,
             sizeof(context->selected_path),
             "%s",
             file->path);
    lv_obj_add_state(file->button, LV_STATE_CHECKED);
    lv_obj_remove_state(context->delete_button, LV_STATE_DISABLED);

    const char *file_name = strrchr(file->path, '/');
    file_name = file_name == NULL ? file->path : file_name + 1;
    const lvgl_language_texts_t *texts =
        lvgl_language_get_texts(context->language);
    lv_label_set_text_fmt(context->status_label,
                          "%s: %s",
                          texts->wav_selected,
                          file_name);
}

/** 为扫描到的一个文件创建可选中的列表按钮。 */
static bool add_file_button(wav_manager_page_context_t *context,
                            const char *file_name)
{
    wav_manager_file_context_t *file = heap_caps_calloc(
        1,
        sizeof(*file),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (file == NULL) {
        return false;
    }

    const int path_length = snprintf(file->path,
                                     sizeof(file->path),
                                     "%s/%s",
                                     WAV_MANAGER_DIRECTORY,
                                     file_name);
    if (path_length < 0 || (size_t)path_length >= sizeof(file->path)) {
        heap_caps_free(file);
        return false;
    }

    lv_obj_t *button = lv_button_create(context->list);
    if (button == NULL) {
        heap_caps_free(file);
        return false;
    }
    file->page_context = context;
    file->button = button;

    lv_obj_set_width(button, LV_PCT(100));
    lv_obj_set_height(button, 42);
    lv_obj_set_style_radius(button, 4, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(button,
                              lv_color_hex(0xB2DFDB),
                              LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(button,
                              lv_color_hex(0xDDE7EE),
                              LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0xB0BEC5), 0);
    lv_obj_add_event_cb(button,
                        file_button_clicked,
                        LV_EVENT_CLICKED,
                        file);
    lv_obj_add_event_cb(button,
                        file_context_deleted,
                        LV_EVENT_DELETE,
                        file);

    lv_obj_t *label = lv_label_create(button);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(label, file_name);
    lv_obj_set_style_text_color(label, lv_color_hex(0x17202A), 0);
    if (context->ui_font != NULL) {
        lv_obj_set_style_text_font(label, context->ui_font, 0);
    }
    lv_obj_center(label);
    return true;
}

/** 把提示文字放入空列表区域，并返回标签供扫描结束时更新或删除。 */
static lv_obj_t *add_list_message(wav_manager_page_context_t *context,
                                  const char *message)
{
    lv_obj_t *label = lv_label_create(context->list);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_text(label, message);
    lv_obj_set_style_text_font(label, interface_font(context), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

/**
 * 每次定时器回调只处理一个目录项。
 *
 * 第一次回调打开目录；后续回调各执行一次 readdir()，并使用 stat() 排除
 * 名称以 .wav 结尾的目录。找到普通 WAV 文件时立即创建按钮。扫描结束后关闭
 * 目录并删除定时器，因此文件数量只影响列表逐渐填充的时间，不再阻塞首帧。
 */
static void scan_file_list(lv_timer_t *timer)
{
    wav_manager_page_context_t *context = lv_timer_get_user_data(timer);
    const lvgl_language_texts_t *texts =
        lvgl_language_get_texts(context->language);

    if (!context->scan_started) {
        context->scan_started = true;
        context->directory = opendir(WAV_MANAGER_DIRECTORY);
        if (context->directory == NULL) {
            lv_label_set_text(context->scan_label,
                              texts->music_directory_error);
            lv_label_set_text(context->status_label,
                              texts->music_directory_error);
            lv_timer_delete(timer);
            context->scan_timer = NULL;
        }
        return;
    }

    struct dirent *entry = readdir(context->directory);
    if (entry == NULL) {
        closedir(context->directory);
        context->directory = NULL;

        if (context->file_count == 0) {
            lv_label_set_text(context->scan_label, texts->music_empty);
        } else if (context->scan_label != NULL) {
            lv_obj_delete(context->scan_label);
            context->scan_label = NULL;
        }

        if (context->completed_message != NULL) {
            lv_label_set_text(context->status_label,
                              context->completed_message);
        } else if (context->file_count == 0) {
            lv_label_set_text(context->status_label, texts->music_empty);
        } else {
            lv_label_set_text(context->status_label,
                              texts->wav_select_file);
        }
        context->completed_message = NULL;
        lv_timer_delete(timer);
        context->scan_timer = NULL;
        return;
    }

    if (!is_wav_file(entry->d_name)) {
        return;
    }

    char path[WAV_MANAGER_PATH_MAX];
    const int path_length = snprintf(path,
                                     sizeof(path),
                                     "%s/%s",
                                     WAV_MANAGER_DIRECTORY,
                                     entry->d_name);
    if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
        return;
    }

    struct stat file_status;
    if (stat(path, &file_status) != 0 || !S_ISREG(file_status.st_mode) ||
        !add_file_button(context, entry->d_name)) {
        return;
    }

    ++context->file_count;
    if (context->scan_label != NULL) {
        lv_obj_delete(context->scan_label);
        context->scan_label = NULL;
    }
}

/**
 * 清空旧文件项并启动一次新的增量扫描。
 *
 * completed_message 用于删除成功后的最终状态；首次进入页面时传 NULL，扫描完成
 * 后状态行恢复为“请选择文件”或“目录为空”。
 */
static void start_file_scan(wav_manager_page_context_t *context,
                            const char *completed_message)
{
    if (context->scan_timer != NULL) {
        lv_timer_delete(context->scan_timer);
        context->scan_timer = NULL;
    }
    if (context->directory != NULL) {
        closedir(context->directory);
        context->directory = NULL;
    }

    clear_selection(context);
    lv_obj_clean(context->list);
    const lvgl_language_texts_t *texts =
        lvgl_language_get_texts(context->language);
    context->scan_started = false;
    context->file_count = 0;
    context->completed_message = completed_message;
    context->scan_label = add_list_message(context, texts->music_loading);
    lv_label_set_text(context->status_label,
                      completed_message != NULL ? completed_message
                                                : texts->music_loading);
    context->scan_timer = lv_timer_create(scan_file_list,
                                          WAV_MANAGER_SCAN_INTERVAL_MS,
                                          context);
    if (context->scan_timer == NULL) {
        lv_label_set_text(context->scan_label, texts->music_directory_error);
        lv_label_set_text(context->status_label,
                          texts->music_directory_error);
    }
}

/** 删除选中的 WAV；成功后重新扫描目录，保证 UI 与 SD 卡内容一致。 */
static void delete_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    wav_manager_page_context_t *context = lv_event_get_user_data(event);
    if (context->selected_path[0] == '\0') {
        return;
    }

    const lvgl_language_texts_t *texts =
        lvgl_language_get_texts(context->language);
    if (unlink(context->selected_path) != 0) {
        ESP_LOGE(TAG,
                 "Cannot delete %s: errno=%d",
                 context->selected_path,
                 errno);
        lv_label_set_text(context->status_label, texts->wav_delete_error);
        return;
    }

    start_file_scan(context, texts->wav_delete_success);
}

/** 返回测试页，并在本次事件分发结束后销毁管理页面。 */
static void close_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    wav_manager_page_context_t *context = lv_event_get_user_data(event);
    lv_screen_load(context->return_page);
    lv_obj_delete_async(context->page);
}

/** 页面首帧绘制完成后才启动首次扫描，让固定界面先响应显示。 */
static void manager_page_drawn(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DRAW_POST) {
        return;
    }

    wav_manager_page_context_t *context = lv_event_get_user_data(event);
    if (!context->initial_scan_requested) {
        context->initial_scan_requested = true;
        context->scan_timer = lv_timer_create(scan_file_list,
                                               WAV_MANAGER_SCAN_INTERVAL_MS,
                                               context);
        if (context->scan_timer == NULL) {
            const lvgl_language_texts_t *texts =
                lvgl_language_get_texts(context->language);
            lv_label_set_text(context->scan_label,
                              texts->music_directory_error);
            lv_label_set_text(context->status_label,
                              texts->music_directory_error);
        }
    }
}

/** 根对象删除时停止扫描并释放页面上下文；文件上下文由按钮删除事件释放。 */
static void manager_page_deleted(lv_event_t *event)
{
    wav_manager_page_context_t *context = lv_event_get_user_data(event);
    if (context->scan_timer != NULL) {
        lv_timer_delete(context->scan_timer);
        context->scan_timer = NULL;
    }
    if (context->directory != NULL) {
        closedir(context->directory);
        context->directory = NULL;
    }
    heap_caps_free(context);
}

lv_obj_t *lvgl_wav_manager_page_create(lv_obj_t *return_page,
                                       lvgl_language_t language,
                                       const lv_font_t *ui_font)
{
    if (return_page == NULL) {
        return NULL;
    }

    wav_manager_page_context_t *context = heap_caps_calloc(
        1,
        sizeof(*context),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (context == NULL) {
        return NULL;
    }
    context->return_page = return_page;
    context->language = language;
    context->ui_font = ui_font;

    lv_obj_t *page = lv_obj_create(NULL);
    if (page == NULL) {
        heap_caps_free(context);
        return NULL;
    }
    context->page = page;
    lv_obj_add_event_cb(page,
                        manager_page_deleted,
                        LV_EVENT_DELETE,
                        context);
    lv_obj_add_event_cb(page,
                        manager_page_drawn,
                        LV_EVENT_DRAW_POST,
                        context);
    lv_obj_set_style_bg_color(page, lv_color_hex(0xF4F6F8), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(page, lv_color_hex(0x17202A), 0);

    const lvgl_language_texts_t *texts = lvgl_language_get_texts(language);
    const lv_font_t *font = interface_font(context);

    lv_obj_t *title = lv_label_create(page);
    lv_label_set_text(title, texts->wav_manager);
    lv_obj_set_style_text_font(title, font, 0);
    lv_obj_set_pos(title, 8, 12);

    context->delete_button = lv_button_create(page);
    lv_obj_set_size(context->delete_button, 68, 40);
    lv_obj_set_pos(context->delete_button, 168, 2);
    lv_obj_set_style_radius(context->delete_button, 4, 0);
    lv_obj_set_style_bg_color(context->delete_button,
                              lv_color_hex(0xC62828),
                              0);
    lv_obj_add_event_cb(context->delete_button,
                        delete_button_clicked,
                        LV_EVENT_CLICKED,
                        context);
    lv_obj_t *delete_label = lv_label_create(context->delete_button);
    lv_label_set_text(delete_label, texts->wav_delete);
    lv_obj_set_style_text_font(delete_label, font, 0);
    lv_obj_center(delete_label);

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
    lv_obj_set_pos(context->status_label, 8, 46);
    lv_obj_set_size(context->status_label, 304, 22);
    lv_label_set_long_mode(context->status_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(context->status_label,
                               ui_font != NULL ? ui_font : font,
                               0);

    context->list = lv_obj_create(page);
    lv_obj_set_pos(context->list, 0, 70);
    lv_obj_set_size(context->list, 320, 170);
    lv_obj_set_style_radius(context->list, 0, 0);
    lv_obj_set_style_border_width(context->list, 0, 0);
    lv_obj_set_style_bg_opa(context->list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(context->list, 4, 0);
    lv_obj_set_style_pad_row(context->list, 4, 0);
    lv_obj_set_flex_flow(context->list, LV_FLEX_FLOW_COLUMN);

    /* 首帧先显示固定控件；DRAW_POST 回调再启动逐项目录扫描。 */
    lv_label_set_text(context->status_label, texts->music_loading);
    context->scan_label = add_list_message(context, texts->music_loading);
    return page;
}
