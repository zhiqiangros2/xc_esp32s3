/**
 * @file lvgl_bluetooth_page.c
 * @brief 循环执行只扫描、不连接的 BLE 浏览，并显示设备快照。
 *
 * 页面与扫描服务的关系：
 *
 *   扫描 5 秒 -> 展示结果 2 秒 -> 再次扫描
 *       ^                              |
 *       |______________________________|
 *
 * 本页面只调用 ble_client_start_scan()，不执行连接或 GATT 操作。页面不从
 * NimBLE 回调直接修改 LVGL；设备快照随页面上下文放在 PSRAM，2 秒刷新回调
 * 不在 LVGL 内部任务栈上创建大数组。离开页面后不再发起下一轮扫描。
 */
#include "lvgl_bluetooth_page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble_client.h"
#include "esp_heap_caps.h"

#define BLUETOOTH_PAGE_REFRESH_INTERVAL_MS 2000U
#define BLUETOOTH_PAGE_RESULT_DISPLAY_MS 2000U
#define BLUETOOTH_PAGE_SCAN_RETRY_MS 500U
#define BLUETOOTH_PAGE_SCAN_CYCLE_MS \
    (BLE_CLIENT_BROWSE_DURATION_MS + BLUETOOTH_PAGE_RESULT_DISPLAY_MS)
#define BLUETOOTH_PAGE_ROW_TEXT_MAX 128U

typedef struct {
    lv_obj_t *container;
    lv_obj_t *label;
    char text[BLUETOOTH_PAGE_ROW_TEXT_MAX];
} bluetooth_device_row_t;

typedef struct {
    lv_obj_t *page;
    lv_obj_t *return_page;
    lv_obj_t *status_label;
    lv_obj_t *list;
    lv_obj_t *empty_label;
    lv_timer_t *refresh_timer;
    lv_timer_t *scan_timer;
    bluetooth_device_row_t rows[BLE_CLIENT_MAX_DEVICES];
    /* 快照约 1.8 KiB，保存在 PSRAM 页面上下文中，避免占用 LVGL 内部栈。 */
    ble_client_snapshot_t snapshot;
    size_t row_count;
    lvgl_language_t language;
    const lv_font_t *ui_font;
    bool refresh_started;
} bluetooth_page_context_t;

/** 中文界面使用完整字体；英文界面回退到 LVGL 默认字体。 */
static const lv_font_t *interface_font(
    const bluetooth_page_context_t *context)
{
    if (context->language == LVGL_LANGUAGE_ZH_CN &&
        context->ui_font != NULL) {
        return context->ui_font;
    }
    return LV_FONT_DEFAULT;
}

/** 按 RSSI 从强到弱排序快照副本，不修改 BSP 内部设备表。 */
static void sort_by_signal_strength(ble_client_snapshot_t *snapshot)
{
    for (size_t index = 1; index < snapshot->device_count; ++index) {
        const ble_client_device_t current = snapshot->devices[index];
        size_t position = index;
        while (position > 0U &&
               snapshot->devices[position - 1U].rssi < current.rssi) {
            snapshot->devices[position] =
                snapshot->devices[position - 1U];
            --position;
        }
        snapshot->devices[position] = current;
    }
}

/** 创建一个固定高度的设备行；后续刷新只修改其标签文字。 */
static bool add_device_row(bluetooth_page_context_t *context)
{
    if (context->row_count >= BLE_CLIENT_MAX_DEVICES) {
        return false;
    }

    bluetooth_device_row_t *row = &context->rows[context->row_count];
    row->container = lv_obj_create(context->list);
    if (row->container == NULL) {
        return false;
    }
    lv_obj_set_width(row->container, LV_PCT(100));
    lv_obj_set_height(row->container, 54);
    lv_obj_set_style_radius(row->container, 4, 0);
    lv_obj_set_style_bg_color(row->container, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(row->container, 1, 0);
    lv_obj_set_style_border_color(row->container, lv_color_hex(0xB0BEC5), 0);
    lv_obj_set_style_pad_all(row->container, 5, 0);
    lv_obj_set_scrollable(row->container, false);

    row->label = lv_label_create(row->container);
    if (row->label == NULL) {
        lv_obj_delete(row->container);
        row->container = NULL;
        return false;
    }
    lv_obj_set_size(row->label, LV_PCT(100), LV_SIZE_CONTENT);
    lv_label_set_long_mode(row->label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(row->label, interface_font(context), 0);
    lv_obj_set_style_text_color(row->label, lv_color_hex(0x17202A), 0);
    lv_obj_align(row->label, LV_ALIGN_LEFT_MID, 0, 0);

    ++context->row_count;
    return true;
}

/** 周期发起浏览扫描；CMD 占用 BLE 时短暂退让并重试。 */
static void start_scan_cycle(lv_timer_t *timer)
{
    const esp_err_t result = ble_client_start_scan();
    lv_timer_set_period(timer,
                        result == ESP_OK
                            ? BLUETOOTH_PAGE_SCAN_CYCLE_MS
                            : BLUETOOTH_PAGE_SCAN_RETRY_MS);
}

/**
 * 周期读取设备快照并刷新可见列表。
 *
 * 一次回调最多创建一个新行；已有行只在文字变化时调用 lv_label_set_text()。
 * 因此附近设备很多时，页面仍能处理触摸和绘制，不会一次性创建 32 组控件。
 */
static void refresh_device_list(lv_timer_t *timer)
{
    bluetooth_page_context_t *context = lv_timer_get_user_data(timer);
    const lvgl_language_texts_t *texts =
        lvgl_language_get_texts(context->language);

    ble_client_snapshot_t *snapshot = &context->snapshot;
    if (ble_client_get_snapshot(snapshot) != ESP_OK) {
        lv_label_set_text(context->status_label, texts->ble_error);
        lv_label_set_text(context->empty_label, texts->ble_error);
        lv_obj_set_hidden(context->empty_label, false);
        return;
    }

    sort_by_signal_strength(snapshot);
    if (!snapshot->ready || snapshot->scanning) {
        lv_label_set_text(context->status_label, texts->ble_scanning);
    } else {
        lv_label_set_text_fmt(context->status_label,
                              "%s: %u",
                              texts->ble_devices,
                              (unsigned)snapshot->device_count);
    }

    if (snapshot->device_count == 0U) {
        lv_label_set_text(context->empty_label,
                          snapshot->ready ? texts->ble_no_devices
                                          : texts->ble_scanning);
        lv_obj_set_hidden(context->empty_label, false);
    } else {
        lv_obj_set_hidden(context->empty_label, true);
    }

    /* 分批建立行；已经建立的隐藏行可以直接重新用于后来出现的设备。 */
    if (context->row_count < snapshot->device_count) {
        (void)add_device_row(context);
    }

    for (size_t index = 0; index < context->row_count; ++index) {
        bluetooth_device_row_t *row = &context->rows[index];
        if (index >= snapshot->device_count) {
            lv_obj_set_hidden(row->container, true);
            continue;
        }

        const ble_client_device_t *device = &snapshot->devices[index];
        const char *name = device->name[0] == '\0'
                               ? texts->ble_unnamed
                               : device->name;
        char new_text[BLUETOOTH_PAGE_ROW_TEXT_MAX];
        if (device->rssi == 127) {
            snprintf(new_text,
                     sizeof(new_text),
                     "%s\n%02X:%02X:%02X:%02X:%02X:%02X    RSSI: N/A",
                     name,
                     device->address[0],
                     device->address[1],
                     device->address[2],
                     device->address[3],
                     device->address[4],
                     device->address[5]);
        } else {
            snprintf(new_text,
                     sizeof(new_text),
                     "%s\n%02X:%02X:%02X:%02X:%02X:%02X    RSSI: %d dBm",
                     name,
                     device->address[0],
                     device->address[1],
                     device->address[2],
                     device->address[3],
                     device->address[4],
                     device->address[5],
                     (int)device->rssi);
        }

        if (strcmp(row->text, new_text) != 0) {
            snprintf(row->text, sizeof(row->text), "%s", new_text);
            lv_label_set_text(row->label, row->text);
        }
        lv_obj_set_hidden(row->container, false);
    }
}

/** 返回测试页，并在事件分发结束后销毁当前页面。 */
static void close_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    bluetooth_page_context_t *context = lv_event_get_user_data(event);
    lv_screen_load(context->return_page);
    lv_obj_delete_async(context->page);
}

/** 首帧绘制完成后再启动快照刷新，保证固定页面先显示。 */
static void bluetooth_page_drawn(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DRAW_POST) {
        return;
    }

    bluetooth_page_context_t *context = lv_event_get_user_data(event);
    if (!context->refresh_started) {
        context->refresh_started = true;
        context->refresh_timer = lv_timer_create(
            refresh_device_list,
            BLUETOOTH_PAGE_REFRESH_INTERVAL_MS,
            context);
        context->scan_timer = lv_timer_create(
            start_scan_cycle,
            BLUETOOTH_PAGE_SCAN_CYCLE_MS,
            context);
        if (context->scan_timer != NULL) {
            /* 进入页面后立即启动第一轮，后续按扫描加展示周期循环。 */
            start_scan_cycle(context->scan_timer);
        }
        if (context->refresh_timer != NULL) {
            /* 下一次 LVGL 处理循环立即读取已有快照，不额外等待 2 秒。 */
            lv_timer_ready(context->refresh_timer);
        }
    }
}

/** 根页面删除时停止 UI 刷新；已经发出的有限时长扫描会自行结束。 */
static void bluetooth_page_deleted(lv_event_t *event)
{
    bluetooth_page_context_t *context = lv_event_get_user_data(event);
    if (context->refresh_timer != NULL) {
        lv_timer_delete(context->refresh_timer);
        context->refresh_timer = NULL;
    }
    if (context->scan_timer != NULL) {
        lv_timer_delete(context->scan_timer);
        context->scan_timer = NULL;
    }
    heap_caps_free(context);
}

lv_obj_t *lvgl_bluetooth_page_create(lv_obj_t *return_page,
                                     lvgl_language_t language,
                                     const lv_font_t *ui_font)
{
    if (return_page == NULL) {
        return NULL;
    }

    bluetooth_page_context_t *context = heap_caps_calloc(
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
                        bluetooth_page_deleted,
                        LV_EVENT_DELETE,
                        context);
    lv_obj_add_event_cb(page,
                        bluetooth_page_drawn,
                        LV_EVENT_DRAW_POST,
                        context);
    lv_obj_set_style_bg_color(page, lv_color_hex(0xF4F6F8), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(page, lv_color_hex(0x17202A), 0);

    const lvgl_language_texts_t *texts = lvgl_language_get_texts(language);
    const lv_font_t *font = interface_font(context);

    lv_obj_t *title = lv_label_create(page);
    lv_label_set_text(title, texts->bluetooth);
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
    lv_obj_set_pos(context->status_label, 8, 46);
    lv_obj_set_size(context->status_label, 304, 22);
    lv_label_set_long_mode(context->status_label, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(context->status_label, texts->ble_scanning);
    lv_obj_set_style_text_font(context->status_label, font, 0);

    context->list = lv_obj_create(page);
    lv_obj_set_pos(context->list, 0, 70);
    lv_obj_set_size(context->list, 320, 170);
    lv_obj_set_style_radius(context->list, 0, 0);
    lv_obj_set_style_border_width(context->list, 0, 0);
    lv_obj_set_style_bg_opa(context->list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(context->list, 4, 0);
    lv_obj_set_style_pad_row(context->list, 4, 0);
    lv_obj_set_flex_flow(context->list, LV_FLEX_FLOW_COLUMN);

    context->empty_label = lv_label_create(context->list);
    lv_obj_set_width(context->empty_label, LV_PCT(100));
    lv_label_set_text(context->empty_label, texts->ble_scanning);
    lv_obj_set_style_text_font(context->empty_label, font, 0);
    lv_obj_set_style_text_align(context->empty_label,
                                LV_TEXT_ALIGN_CENTER,
                                0);
    return page;
}
