/**
 * @file lvgl_wav_manager_page.c
 * @brief /sdcard/music 目录下 WAV 文件的选择和删除页面。
 *
 * 页面只在创建和删除成功后扫描目录。创建函数返回前已经完成首次扫描，因此
 * 调用者加载页面时可以直接显示文件列表；关闭根对象后，文件按钮、按钮上下文
 * 和页面上下文会随 LVGL 对象树一起释放。
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

#include "esp_log.h"
#include "sd_fatfs.h"

#define WAV_MANAGER_DIRECTORY SD_MOUNT_POINT "/music"
#define WAV_MANAGER_PATH_MAX 384U

typedef struct {
    lv_obj_t *page;
    lv_obj_t *return_page;
    lv_obj_t *list;
    lv_obj_t *status_label;
    lv_obj_t *delete_button;
    lv_obj_t *selected_button;
    lvgl_language_t language;
    const lv_font_t *ui_font;
    char selected_path[WAV_MANAGER_PATH_MAX];
} wav_manager_page_context_t;

typedef struct {
    wav_manager_page_context_t *page_context;
    lv_obj_t *button;
    char path[WAV_MANAGER_PATH_MAX];
} wav_manager_file_context_t;

/* 扫描期间保存文件名；关闭目录后才创建 LVGL 控件。 */
typedef struct wav_manager_file_entry {
    struct wav_manager_file_entry *next;
    char name[];
} wav_manager_file_entry_t;

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
    free(lv_event_get_user_data(event));
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
    wav_manager_file_context_t *file = calloc(1, sizeof(*file));
    if (file == NULL) {
        return false;
    }

    const int path_length = snprintf(file->path,
                                     sizeof(file->path),
                                     "%s/%s",
                                     WAV_MANAGER_DIRECTORY,
                                     file_name);
    if (path_length < 0 || (size_t)path_length >= sizeof(file->path)) {
        free(file);
        return false;
    }

    lv_obj_t *button = lv_button_create(context->list);
    if (button == NULL) {
        free(file);
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

/** 把提示文字放入空列表区域。 */
static void add_list_message(wav_manager_page_context_t *context,
                             const char *message)
{
    lv_obj_t *label = lv_label_create(context->list);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_text(label, message);
    lv_obj_set_style_text_font(label, interface_font(context), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

/**
 * 先完整扫描目录并复制文件名，关闭目录后再创建文件按钮。
 *
 * 额外使用 stat() 排除名称以 .wav 结尾的目录；路径过长或内存分配失败的
 * 条目会被跳过，不会产生截断路径供删除操作使用。
 */
static size_t populate_file_list(wav_manager_page_context_t *context,
                                 bool *directory_opened)
{
    *directory_opened = false;
    DIR *directory = opendir(WAV_MANAGER_DIRECTORY);
    if (directory == NULL) {
        const lvgl_language_texts_t *texts =
            lvgl_language_get_texts(context->language);
        add_list_message(context, texts->music_directory_error);
        return 0;
    }
    *directory_opened = true;

    wav_manager_file_entry_t *file_list = NULL;
    wav_manager_file_entry_t *file_list_tail = NULL;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!is_wav_file(entry->d_name)) {
            continue;
        }

        char path[WAV_MANAGER_PATH_MAX];
        const int path_length = snprintf(path,
                                         sizeof(path),
                                         "%s/%s",
                                         WAV_MANAGER_DIRECTORY,
                                         entry->d_name);
        if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
            continue;
        }

        struct stat file_status;
        if (stat(path, &file_status) != 0 || !S_ISREG(file_status.st_mode)) {
            continue;
        }

        const size_t name_length = strlen(entry->d_name);
        wav_manager_file_entry_t *file =
            malloc(sizeof(*file) + name_length + 1U);
        if (file == NULL) {
            continue;
        }
        file->next = NULL;
        memcpy(file->name, entry->d_name, name_length + 1U);

        if (file_list_tail == NULL) {
            file_list = file;
        } else {
            file_list_tail->next = file;
        }
        file_list_tail = file;
    }
    closedir(directory);

    size_t count = 0;
    while (file_list != NULL) {
        wav_manager_file_entry_t *file = file_list;
        file_list = file->next;
        if (add_file_button(context, file->name)) {
            ++count;
        }
        free(file);
    }

    if (count == 0) {
        const lvgl_language_texts_t *texts =
            lvgl_language_get_texts(context->language);
        add_list_message(context, texts->music_empty);
    }
    return count;
}

/** 清空旧文件项，重新扫描目录，并更新状态行。 */
static void rebuild_file_list(wav_manager_page_context_t *context,
                              const char *completed_message)
{
    clear_selection(context);
    lv_obj_clean(context->list);

    bool directory_opened = false;
    const size_t file_count =
        populate_file_list(context, &directory_opened);
    const lvgl_language_texts_t *texts =
        lvgl_language_get_texts(context->language);

    if (!directory_opened) {
        lv_label_set_text(context->status_label,
                          texts->music_directory_error);
    } else if (completed_message != NULL) {
        lv_label_set_text(context->status_label, completed_message);
    } else if (file_count == 0) {
        lv_label_set_text(context->status_label, texts->music_empty);
    } else {
        lv_label_set_text(context->status_label, texts->wav_select_file);
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

    rebuild_file_list(context, texts->wav_delete_success);
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

/** 根对象删除时释放页面上下文；文件上下文由各按钮删除事件释放。 */
static void manager_page_deleted(lv_event_t *event)
{
    free(lv_event_get_user_data(event));
}

lv_obj_t *lvgl_wav_manager_page_create(lv_obj_t *return_page,
                                       lvgl_language_t language,
                                       const lv_font_t *ui_font)
{
    if (return_page == NULL) {
        return NULL;
    }

    wav_manager_page_context_t *context = calloc(1, sizeof(*context));
    if (context == NULL) {
        return NULL;
    }
    context->return_page = return_page;
    context->language = language;
    context->ui_font = ui_font;

    lv_obj_t *page = lv_obj_create(NULL);
    if (page == NULL) {
        free(context);
        return NULL;
    }
    context->page = page;
    lv_obj_add_event_cb(page,
                        manager_page_deleted,
                        LV_EVENT_DELETE,
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

    /* 调用者尚未加载页面；这里完成扫描后，首次显示就是完整文件列表。 */
    rebuild_file_list(context, NULL);
    return page;
}
