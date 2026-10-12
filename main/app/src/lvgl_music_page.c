/**
 * @file lvgl_music_page.c
 * @brief SD 卡 WAV 文件列表和播放控制页面。
 *
 * 本文件只运行在 LVGL 任务上下文中。页面先完成切换，再通过 LVGL 定时器
 * 每次读取一个 /sdcard/music 目录项，使大目录扫描不会阻塞首次画面显示。
 * WAV 解析、文件读取和 I2S 写入仍由 wav_player.c 的后台任务执行。
 *
 * 内存归属：页面拥有 music_page_context_t；每个曲目按钮分别拥有一个
 * music_track_context_t。对象收到 LV_EVENT_DELETE 时释放对应上下文。
 */
#include "lvgl_music_page.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sd_fatfs.h"
#include "wav_player.h"

/* 只枚举 SD 卡挂载点下的 music 目录，不递归扫描子目录。 */
#define MUSIC_DIRECTORY SD_MOUNT_POINT "/music"
/* 完整曲目路径容量，必须与 wav_player.c 接受的路径上限保持一致。 */
#define MUSIC_PATH_MAX 384
/* 每 250 ms 拉取一次后台状态；兼顾界面响应和 LVGL 任务开销。 */
#define MUSIC_STATUS_UPDATE_MS 250
/* 每次扫描一个目录项，5 ms 后继续，让 LVGL 有机会处理绘制和触摸。 */
#define MUSIC_SCAN_INTERVAL_MS 5

/* 音乐页面整个生命周期共享的 UI 状态。 */
typedef struct {
    /* 当前音乐页面根对象，删除它会触发上下文清理回调。 */
    lv_obj_t *page;
    /* 点击关闭后要返回的已有页面；本页面不拥有也不释放它。 */
    lv_obj_t *return_page;
    /* 显示“加载、播放、停止、错误”的单行标签。 */
    lv_obj_t *status_label;
    /* 周期读取 wav_player_status_t 的 LVGL 定时器。 */
    lv_timer_t *status_timer;
    /* 页面显示后逐项扫描 SD 音乐目录的 LVGL 定时器。 */
    lv_timer_t *scan_timer;
    /* 正在扫描的 FATFS 目录，扫描结束或页面关闭时必须 closedir()。 */
    DIR *music_directory;
    /* 曲目按钮的父对象及扫描期间显示的“加载中”标签。 */
    lv_obj_t *track_list;
    lv_obj_t *scan_label;
    /* 防止页面后续重绘时再次打开目录。 */
    bool scan_started;
    /* 已经成功创建的曲目按钮数量。 */
    size_t track_count;
    /* 创建页面时固定的界面语言。 */
    lvgl_language_t language;
    /* 可选中文字体；为 NULL 时回退到 LV_FONT_DEFAULT。 */
    const lv_font_t *ui_font;
    /* 最近一次已经绘制的状态，用于跳过无变化的文本更新。 */
    wav_player_state_t displayed_state;
    /* 最近一次已经绘制的文件名，与 displayed_state 一起用于去重。 */
    char displayed_file[WAV_PLAYER_FILE_NAME_MAX];
} music_page_context_t;

/* 每个曲目按钮独立保存的数据，由该按钮的删除事件释放。 */
typedef struct {
    /* 回指页面上下文，用于取得状态标签、语言和字体。 */
    music_page_context_t *page_context;
    /* 点击时直接提交给播放器的完整文件路径。 */
    char path[MUSIC_PATH_MAX];
} music_track_context_t;

/** 根据页面语言选择字体；中文优先使用调用者传入的完整字库。 */
static const lv_font_t *interface_font(const music_page_context_t *context)
{
    if (context->language == LVGL_LANGUAGE_ZH_CN &&
        context->ui_font != NULL) {
        return context->ui_font;
    }
    return LV_FONT_DEFAULT;
}

/**
 * @brief 判断目录项名称是否以不区分大小写的 .wav 结尾。
 *
 * 这里只根据扩展名筛选列表；文件内容是否为受支持的 PCM 格式由后台播放器
 * 在点击后解析并校验。
 */
static bool is_wav_file(const char *name)
{
    const char *extension = strrchr(name, '.');
    if (extension == NULL) {
        return false;
    }

    return (extension[1] == 'w' || extension[1] == 'W') &&
           (extension[2] == 'a' || extension[2] == 'A') &&
           (extension[3] == 'v' || extension[3] == 'V') &&
           extension[4] == '\0';
}

/**
 * @brief 从后台播放器取得状态并更新页面状态标签。
 *
 * 该函数由页面创建流程和定时器调用。只有状态或文件名发生变化时才调用
 * lv_label_set_text*()，避免每 250 ms 重建文本、触发布局和无意义重绘。
 */
static void update_status_label(music_page_context_t *context)
{
    wav_player_status_t status;
    if (wav_player_get_status(&status) != ESP_OK) {
        return;
    }
    /* 状态与文件名均未变化时，保持现有 LVGL 文本对象不动。 */
    if (status.state == context->displayed_state &&
        strcmp(status.file_name, context->displayed_file) == 0) {
        return;
    }

    const lvgl_language_texts_t *texts =
        lvgl_language_get_texts(context->language);
    /* 所有可见字符串都来自当前语言表，不在业务逻辑中硬编码。 */
    switch (status.state) {
        case WAV_PLAYER_LOADING:
            lv_label_set_text_fmt(context->status_label,
                                  "%s: %s",
                                  texts->music_loading,
                                  status.file_name);
            break;
        case WAV_PLAYER_PLAYING:
            lv_label_set_text_fmt(context->status_label,
                                  "%s: %s",
                                  texts->music_playing,
                                  status.file_name);
            break;
        case WAV_PLAYER_ERROR:
            lv_label_set_text_fmt(context->status_label,
                                  "%s: %s",
                                  texts->music_error,
                                  status.file_name);
            break;
        case WAV_PLAYER_STOPPED:
        default:
            lv_label_set_text(context->status_label, texts->music_stopped);
            break;
    }

    /* 文本更新成功后保存本次快照，供下一轮定时器去重。 */
    context->displayed_state = status.state;
    snprintf(context->displayed_file,
             sizeof(context->displayed_file),
             "%s",
             status.file_name);
}

/** LVGL 定时器回调：user_data 始终指向仍然存活的页面上下文。 */
static void status_timer_elapsed(lv_timer_t *timer)
{
    music_page_context_t *context = lv_timer_get_user_data(timer);
    update_status_label(context);
}

/**
 * @brief 页面根对象删除回调。
 *
 * 先删除两个定时器并关闭尚未扫描完的目录，保证释放 context 后不会再有
 * 回调或 FATFS 句柄访问它。曲目按钮上下文由各自的删除事件释放。
 */
static void music_page_deleted(lv_event_t *event)
{
    music_page_context_t *context = lv_event_get_user_data(event);
    if (context->scan_timer != NULL) {
        lv_timer_delete(context->scan_timer);
        context->scan_timer = NULL;
    }
    if (context->status_timer != NULL) {
        lv_timer_delete(context->status_timer);
        context->status_timer = NULL;
    }
    if (context->music_directory != NULL) {
        closedir(context->music_directory);
        context->music_directory = NULL;
    }
    free(context);
}

/** 曲目按钮删除回调：释放 add_track_button() 为该按钮分配的上下文。 */
static void track_context_deleted(lv_event_t *event)
{
    free(lv_event_get_user_data(event));
}

/**
 * @brief 曲目按钮点击回调。
 *
 * 先立即把标签改为“加载中”，让用户无需等待下一次 250 ms 状态轮询；随后仅
 * 向后台任务提交路径。若命令提交本身失败，立即显示错误状态。文件不存在、
 * 格式不支持等异步错误由状态定时器在播放器返回后显示。
 */
static void track_button_clicked(lv_event_t *event)
{
    music_track_context_t *track = lv_event_get_user_data(event);
    music_page_context_t *context = track->page_context;
    const lvgl_language_texts_t *texts =
        lvgl_language_get_texts(context->language);
    const char *file_name = strrchr(track->path, '/');
    file_name = file_name == NULL ? track->path : file_name + 1;

    /* 先立即更新状态标签，再向后台播放器消息队列发送播放事件。 */
    lv_label_set_text_fmt(context->status_label,
                          "%s: %s",
                          texts->music_loading,
                          file_name);
    context->displayed_state = WAV_PLAYER_LOADING;
    /* 保存与标签一致的文件名，防止定时器马上重复设置同一段文本。 */
    const size_t displayed_name_length =
        strlen(file_name) < sizeof(context->displayed_file) - 1
            ? strlen(file_name)
            : sizeof(context->displayed_file) - 1;
    memcpy(context->displayed_file, file_name, displayed_name_length);
    context->displayed_file[displayed_name_length] = '\0';
    lv_obj_invalidate(context->status_label);

    /* wav_player_play() 只投递命令，不会在 LVGL 回调中打开或读取文件。 */
    const esp_err_t result = wav_player_play(track->path);
    if (result != ESP_OK) {
        lv_label_set_text(context->status_label, texts->music_error);
        context->displayed_state = WAV_PLAYER_ERROR;
        context->displayed_file[0] = '\0';
    }
}

/** 停止按钮只发送异步停止命令，状态标签由定时器更新为“已停止”。 */
static void stop_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        (void)wav_player_stop();
    }
}

/**
 * @brief 关闭按钮停止播放、切回来源页面，再异步删除当前页面。
 *
 * 使用 lv_obj_delete_async()，避免在当前事件仍沿对象树分发时立即销毁事件
 * 来源及其父对象。return_page 由上层拥有，切屏不会释放它。
 */
static void close_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    music_page_context_t *context = lv_event_get_user_data(event);
    (void)wav_player_stop();
    lv_screen_load(context->return_page);
    lv_obj_delete_async(context->page);
}

/**
 * @brief 为一个 WAV 目录项创建列表按钮和文件名标签。
 *
 * track 上下文成功绑定到按钮后由 LV_EVENT_DELETE 管理；此前任一创建或路径
 * 格式化步骤失败，则在本函数中立即释放。长文件名使用省略号显示，但保存和
 * 提交给播放器的仍是完整路径。
 *
 * @return true 创建成功；false 表示内存不足、路径过长或 LVGL 创建失败。
 */
static bool add_track_button(lv_obj_t *list,
                             music_page_context_t *page_context,
                             const char *file_name)
{
    music_track_context_t *track = calloc(1, sizeof(*track));
    if (track == NULL) {
        return false;
    }

    track->page_context = page_context;
    /* snprintf 的返回值可检测完整路径是否超出固定命令容量。 */
    const int path_length = snprintf(track->path,
                                     sizeof(track->path),
                                     "%s/%s",
                                     MUSIC_DIRECTORY,
                                     file_name);
    if (path_length < 0 ||
        (size_t)path_length >= sizeof(track->path)) {
        free(track);
        return false;
    }

    lv_obj_t *button = lv_button_create(list);
    if (button == NULL) {
        free(track);
        return false;
    }

    lv_obj_set_size(button, LV_PCT(100), 44);
    lv_obj_set_style_radius(button, 4, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xDDE7EE),
                              LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0xB0BEC5), 0);
    /* 点击回调使用 track；删除回调负责最终释放同一个 track。 */
    lv_obj_add_event_cb(button,
                        track_button_clicked,
                        LV_EVENT_CLICKED,
                        track);
    lv_obj_add_event_cb(button,
                        track_context_deleted,
                        LV_EVENT_DELETE,
                        track);

    /* 标签宽度占满按钮，超长文件名在末尾显示省略号，不撑大布局。 */
    lv_obj_t *label = lv_label_create(button);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(label, file_name);
    lv_obj_set_style_text_color(label, lv_color_hex(0x17202A), 0);
    if (page_context->ui_font != NULL) {
        lv_obj_set_style_text_font(label, page_context->ui_font, 0);
    }
    lv_obj_center(label);
    return true;
}

/**
 * @brief 每次定时器回调只处理一个目录项。
 *
 * 第一次回调打开目录，后续回调各执行一次 readdir()。发现 WAV 时立即创建
 * 对应按钮；扫描完成后关闭目录并删除本定时器。整个过程不使用临时文件名
 * 链表，也不会在页面创建函数中一次性遍历完整目录。
 */
static void scan_music_directory(lv_timer_t *timer)
{
    music_page_context_t *context = lv_timer_get_user_data(timer);
    const lvgl_language_texts_t *texts =
        lvgl_language_get_texts(context->language);

    if (!context->scan_started) {
        context->scan_started = true;
        context->music_directory = opendir(MUSIC_DIRECTORY);
        if (context->music_directory == NULL) {
            lv_label_set_text(context->scan_label,
                              texts->music_directory_error);
            lv_timer_delete(timer);
            context->scan_timer = NULL;
        }
        return;
    }

    struct dirent *entry = readdir(context->music_directory);
    if (entry == NULL) {
        closedir(context->music_directory);
        context->music_directory = NULL;

        if (context->track_count == 0) {
            lv_label_set_text(context->scan_label, texts->music_empty);
        } else if (context->scan_label != NULL) {
            lv_obj_delete(context->scan_label);
            context->scan_label = NULL;
        }

        lv_timer_delete(timer);
        context->scan_timer = NULL;
        return;
    }

    if (!is_wav_file(entry->d_name) ||
        !add_track_button(context->track_list, context, entry->d_name)) {
        return;
    }

    ++context->track_count;
    if (context->scan_label != NULL) {
        lv_obj_delete(context->scan_label);
        context->scan_label = NULL;
    }
}

/** 页面首帧绘制完成后才启动目录扫描，保证固定界面先显示出来。 */
static void music_page_drawn(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DRAW_POST) {
        return;
    }

    music_page_context_t *context = lv_event_get_user_data(event);
    if (context->scan_timer == NULL && !context->scan_started) {
        context->scan_timer = lv_timer_create(scan_music_directory,
                                               MUSIC_SCAN_INTERVAL_MS,
                                               context);
    }
}

/**
 * @brief 创建音乐页面；页面显示后再逐项扫描 SD 卡曲目。
 *
 * 页面布局固定适配 320x240：顶部标题和操作按钮，中部一行状态，底部为可滚动
 * 曲目列表。创建成功后页面尚未自动加载，由调用者决定何时切换到该对象。
 *
 * @param return_page 关闭页面后返回的目标页面，不能为 NULL。
 * @param language 页面使用的语言。
 * @param ui_font 可选界面字体，中文环境应传入包含所需字形的字体。
 * @return 新页面对象；分配或创建失败返回 NULL。
 */
lv_obj_t *lvgl_music_page_create(lv_obj_t *return_page,
                                 lvgl_language_t language,
                                 const lv_font_t *ui_font)
{
    if (return_page == NULL) {
        return NULL;
    }

    /* context 与页面同寿命，根对象删除回调负责释放。 */
    music_page_context_t *context = calloc(1, sizeof(*context));
    if (context == NULL) {
        return NULL;
    }
    context->return_page = return_page;
    context->language = language;
    context->ui_font = ui_font;
    /* 使用枚举范围外的初值，强制首次 update_status_label() 写入文本。 */
    context->displayed_state = (wav_player_state_t)-1;

    /* parent=NULL 创建可作为 screen 加载的根对象。 */
    lv_obj_t *page = lv_obj_create(NULL);
    if (page == NULL) {
        free(context);
        return NULL;
    }
    context->page = page;
    lv_obj_add_event_cb(page,
                        music_page_deleted,
                        LV_EVENT_DELETE,
                        context);
    lv_obj_add_event_cb(page,
                        music_page_drawn,
                        LV_EVENT_DRAW_POST,
                        context);
    lv_obj_set_style_bg_color(page, lv_color_hex(0xF4F6F8), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(page, lv_color_hex(0x17202A), 0);

    const lvgl_language_texts_t *texts = lvgl_language_get_texts(language);
    const lv_font_t *font = interface_font(context);

    /* 顶部控制区：标题、停止按钮和关闭按钮。 */
    lv_obj_t *title = lv_label_create(page);
    lv_label_set_text(title, texts->music);
    lv_obj_set_style_text_font(title, font, 0);
    lv_obj_set_pos(title, 8, 12);

    lv_obj_t *stop_button = lv_button_create(page);
    lv_obj_set_size(stop_button, 68, 40);
    lv_obj_set_pos(stop_button, 168, 2);
    lv_obj_set_style_radius(stop_button, 4, 0);
    lv_obj_set_style_bg_color(stop_button, lv_color_hex(0xC62828), 0);
    lv_obj_add_event_cb(stop_button,
                        stop_button_clicked,
                        LV_EVENT_CLICKED,
                        NULL);
    lv_obj_t *stop_label = lv_label_create(stop_button);
    lv_label_set_text(stop_label, texts->stop);
    lv_obj_set_style_text_font(stop_label, font, 0);
    lv_obj_center(stop_label);

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

    /* 状态行限制为单行宽度，长文件名以省略号显示。 */
    context->status_label = lv_label_create(page);
    lv_obj_set_pos(context->status_label, 8, 46);
    lv_obj_set_size(context->status_label, 304, 22);
    lv_label_set_long_mode(context->status_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(context->status_label,
                               ui_font != NULL ? ui_font : font,
                               0);

    /* 下方列表使用纵向 Flex，超过可视高度后由 LVGL 提供滚动。 */
    context->track_list = lv_obj_create(page);
    lv_obj_set_pos(context->track_list, 0, 70);
    lv_obj_set_size(context->track_list, 320, 170);
    lv_obj_set_style_radius(context->track_list, 0, 0);
    lv_obj_set_style_border_width(context->track_list, 0, 0);
    lv_obj_set_style_bg_opa(context->track_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(context->track_list, 4, 0);
    lv_obj_set_style_pad_row(context->track_list, 4, 0);
    lv_obj_set_flex_flow(context->track_list, LV_FLEX_FLOW_COLUMN);

    /* 页面先显示加载提示；首帧绘制完成后再启动逐项目录扫描。 */
    context->scan_label = lv_label_create(context->track_list);
    lv_label_set_text(context->scan_label, texts->music_loading);
    lv_obj_set_style_text_font(context->scan_label, font, 0);

    /* 先显示当前播放器状态，再创建周期同步定时器。 */
    update_status_label(context);
    context->status_timer = lv_timer_create(status_timer_elapsed,
                                             MUSIC_STATUS_UPDATE_MS,
                                             context);
    return page;
}
