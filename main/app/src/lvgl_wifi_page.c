/**
 * @file lvgl_wifi_page.c
 * @brief WiFi STA 外网和 SoftAP 共享上网配置页面。
 *
 * 页面打开时创建 3 个组合配置按钮。每个按钮同时选择同一序号的 STA 和 AP，
 * 上方两栏显示当前 STA/AP 的 SSID、密码和连接状态。标题栏的删除按钮清除
 * Flash 凭据并停止 WiFi，关闭按钮只销毁页面。页面每秒从 BSP 读取一次状态；
 * 单纯关闭页面不会影响 STA 自动重连、SoftAP 或已连接客户端。
 */
#include "lvgl_wifi_page.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "wifi_manager.h"

#define WIFI_PAGE_REFRESH_INTERVAL_MS 1000U
typedef struct wifi_page_context wifi_page_context_t;

typedef struct {
    wifi_page_context_t *context;
    size_t profile_index;
} wifi_button_action_t;

struct wifi_page_context {
    lv_obj_t *page;
    lv_obj_t *return_page;
    lv_obj_t *sta_status_label;
    lv_obj_t *ap_status_label;
    lv_obj_t *profile_buttons[WIFI_MANAGER_PROFILE_COUNT];
    wifi_button_action_t profile_actions[WIFI_MANAGER_PROFILE_COUNT];
    lv_timer_t *refresh_timer;
    lvgl_language_t language;
    const lv_font_t *ui_font;
    bool refresh_started;
};

static const char *TAG = "WIFI_PAGE";

/** 中文界面使用完整字体；英文界面使用 LVGL 默认字体。 */
static const lv_font_t *interface_font(const wifi_page_context_t *context)
{
    if (context->language == LVGL_LANGUAGE_ZH_CN &&
        context->ui_font != NULL) {
        return context->ui_font;
    }
    return LV_FONT_DEFAULT;
}

/** 更新当前账号、密码、连接状态及选中按钮；本函数只在 LVGL 任务中调用。 */
static void refresh_wifi_status(lv_timer_t *timer)
{
    wifi_page_context_t *context = lv_timer_get_user_data(timer);
    const lvgl_language_texts_t *texts =
        lvgl_language_get_texts(context->language);
    wifi_manager_status_t status;
    if (wifi_manager_get_status(&status) != ESP_OK || !status.ready) {
        lv_label_set_text_fmt(context->sta_status_label,
                              "STA\n%s",
                              texts->wifi_unavailable);
        lv_label_set_text_fmt(context->ap_status_label,
                              "AP\n%s",
                              texts->wifi_unavailable);
        return;
    }

    const char *sta_password = NULL;
    const char *ap_password = NULL;
    for (size_t index = 0; index < WIFI_MANAGER_PROFILE_COUNT; ++index) {
        const char *sta_ssid = wifi_manager_get_sta_profile_ssid(index);
        const char *ap_ssid = wifi_manager_get_ap_profile_ssid(index);
        if (status.sta_configured && sta_ssid != NULL &&
            strcmp(status.sta_ssid, sta_ssid) == 0) {
            sta_password = wifi_manager_get_sta_profile_password(index);
        }
        if (status.ap_configured && ap_ssid != NULL &&
            strcmp(status.ap_ssid, ap_ssid) == 0) {
            ap_password = wifi_manager_get_ap_profile_password(index);
        }
    }

    if (!status.sta_configured) {
        lv_label_set_text_fmt(context->sta_status_label,
                              "STA: --\nPWD: --\n%s",
                              texts->wifi_not_selected);
    } else if (status.sta_connected) {
        lv_label_set_text_fmt(context->sta_status_label,
                              "STA: %s\nPWD: %s\nIP: %s",
                              status.sta_ssid,
                              sta_password != NULL ? sta_password : "--",
                              status.sta_ip);
    } else {
        lv_label_set_text_fmt(context->sta_status_label,
                              "STA: %s\nPWD: %s\n%s",
                              status.sta_ssid,
                              sta_password != NULL ? sta_password : "--",
                              status.sta_reconnecting
                                  ? texts->wifi_reconnecting
                                  : texts->wifi_connecting);
    }

    if (!status.ap_configured) {
        lv_label_set_text_fmt(context->ap_status_label,
                              "AP: --\nPWD: --\n%s",
                              texts->wifi_not_selected);
    } else if (!status.ap_started) {
        lv_label_set_text_fmt(context->ap_status_label,
                              "AP: %s\nPWD: %s\n%s",
                              status.ap_ssid,
                              ap_password != NULL ? ap_password : "--",
                              texts->wifi_connecting);
    } else if (status.sta_connected && status.napt_enabled) {
        lv_label_set_text_fmt(context->ap_status_label,
                              "AP: %s\nPWD: %s\n%s:%u",
                              status.ap_ssid,
                              ap_password != NULL ? ap_password : "--",
                              texts->wifi_clients,
                              (unsigned)status.ap_client_count);
    } else {
        lv_label_set_text_fmt(context->ap_status_label,
                              "AP: %s\nPWD: %s\n%s",
                              status.ap_ssid,
                              ap_password != NULL ? ap_password : "--",
                              texts->wifi_waiting_sta);
    }

    for (size_t index = 0; index < WIFI_MANAGER_PROFILE_COUNT; ++index) {
        const char *sta_ssid = wifi_manager_get_sta_profile_ssid(index);
        const char *ap_ssid = wifi_manager_get_ap_profile_ssid(index);
        const bool profile_selected =
            status.sta_configured && status.ap_configured &&
            sta_ssid != NULL && ap_ssid != NULL &&
            strcmp(status.sta_ssid, sta_ssid) == 0 &&
            strcmp(status.ap_ssid, ap_ssid) == 0;
        if (profile_selected) {
            lv_obj_add_state(context->profile_buttons[index],
                             LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(context->profile_buttons[index],
                                LV_STATE_CHECKED);
        }
    }
}

/** 把组合配置序号发给 BSP；NVS 与 WiFi 操作由内部 RAM 栈任务异步完成。 */
static void profile_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    wifi_button_action_t *action = lv_event_get_user_data(event);
    const esp_err_t result =
        wifi_manager_select_profile(action->profile_index);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "Cannot select WiFi profile %u: %s",
                 (unsigned)(action->profile_index + 1U),
                 esp_err_to_name(result));
    }
    if (action->context->refresh_timer != NULL) {
        lv_timer_ready(action->context->refresh_timer);
    }
}

/** 异步删除 Flash 中的 STA/AP 凭据；具体 NVS 操作由 BSP 任务执行。 */
static void clear_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    wifi_page_context_t *context = lv_event_get_user_data(event);
    const esp_err_t result = wifi_manager_clear_profiles();
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "Cannot clear WiFi profiles: %s",
                 esp_err_to_name(result));
    }
    if (context->refresh_timer != NULL) {
        lv_timer_ready(context->refresh_timer);
    }
}

/** 返回测试页并异步销毁 WiFi 页面，网络服务不受影响。 */
static void close_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    wifi_page_context_t *context = lv_event_get_user_data(event);
    lv_screen_load(context->return_page);
    lv_obj_delete_async(context->page);
}

/** 首帧显示后再启动状态刷新，避免创建页面时额外读取状态。 */
static void wifi_page_drawn(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DRAW_POST) {
        return;
    }

    wifi_page_context_t *context = lv_event_get_user_data(event);
    if (!context->refresh_started) {
        context->refresh_started = true;
        context->refresh_timer = lv_timer_create(refresh_wifi_status,
                                                  WIFI_PAGE_REFRESH_INTERVAL_MS,
                                                  context);
        if (context->refresh_timer != NULL) {
            lv_timer_ready(context->refresh_timer);
        }
    }
}

/** 页面销毁时释放 UI 自己的资源；不调用任何 WiFi 停止接口。 */
static void wifi_page_deleted(lv_event_t *event)
{
    wifi_page_context_t *context = lv_event_get_user_data(event);
    if (context->refresh_timer != NULL) {
        lv_timer_delete(context->refresh_timer);
        context->refresh_timer = NULL;
    }
    heap_caps_free(context);
}

/** 创建一个同时包含 STA 与 AP 名称的组合配置按钮。 */
static lv_obj_t *create_profile_button(lv_obj_t *page,
                                       int32_t y,
                                       size_t profile_index,
                                       const char *sta_ssid,
                                       const char *ap_ssid,
                                       const lv_font_t *font,
                                       wifi_button_action_t *action)
{
    lv_obj_t *button = lv_button_create(page);
    if (button == NULL) {
        return NULL;
    }
    lv_obj_set_size(button, 312, 32);
    lv_obj_set_pos(button, 4, y);
    lv_obj_set_style_radius(button, 4, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x1565C0), 0);
    lv_obj_set_style_bg_color(button,
                              lv_color_hex(0x2E7D32),
                              LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(button,
                              lv_color_hex(0x263238),
                              LV_STATE_PRESSED);
    lv_obj_add_event_cb(button,
                        profile_button_clicked,
                        LV_EVENT_CLICKED,
                        action);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text_fmt(label,
                          "%u  %s + %s",
                          (unsigned)(profile_index + 1U),
                          sta_ssid,
                          ap_ssid);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(label);
    return button;
}

lv_obj_t *lvgl_wifi_page_create(lv_obj_t *return_page,
                                lvgl_language_t language,
                                const lv_font_t *ui_font)
{
    if (return_page == NULL) {
        return NULL;
    }

    wifi_page_context_t *context = heap_caps_calloc(
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
    lv_obj_add_event_cb(page, wifi_page_deleted, LV_EVENT_DELETE, context);
    lv_obj_add_event_cb(page, wifi_page_drawn, LV_EVENT_DRAW_POST, context);
    lv_obj_set_style_bg_color(page, lv_color_hex(0xF4F6F8), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(page, lv_color_hex(0x17202A), 0);

    const lvgl_language_texts_t *texts = lvgl_language_get_texts(language);
    const lv_font_t *font = interface_font(context);

    lv_obj_t *title = lv_label_create(page);
    lv_label_set_text(title, texts->wifi);
    lv_obj_set_style_text_font(title, font, 0);
    lv_obj_set_pos(title, 8, 12);

    lv_obj_t *clear_button = lv_button_create(page);
    lv_obj_set_size(clear_button, 76, 40);
    lv_obj_set_pos(clear_button, 164, 2);
    lv_obj_set_style_radius(clear_button, 4, 0);
    lv_obj_set_style_bg_color(clear_button, lv_color_hex(0xC62828), 0);
    lv_obj_add_event_cb(clear_button,
                        clear_button_clicked,
                        LV_EVENT_CLICKED,
                        context);
    lv_obj_t *clear_label = lv_label_create(clear_button);
    lv_label_set_text(clear_label, texts->wifi_clear);
    lv_obj_set_style_text_font(clear_label, font, 0);
    lv_obj_center(clear_label);

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

    context->sta_status_label = lv_label_create(page);
    lv_obj_set_pos(context->sta_status_label, 4, 42);
    lv_obj_set_size(context->sta_status_label, 154, 90);
    lv_label_set_long_mode(context->sta_status_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(context->sta_status_label, font, 0);
    lv_label_set_text(context->sta_status_label, "STA");

    context->ap_status_label = lv_label_create(page);
    lv_obj_set_pos(context->ap_status_label, 162, 42);
    lv_obj_set_size(context->ap_status_label, 154, 90);
    lv_label_set_long_mode(context->ap_status_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(context->ap_status_label, font, 0);
    lv_label_set_text(context->ap_status_label, "AP");

    for (size_t index = 0; index < WIFI_MANAGER_PROFILE_COUNT; ++index) {
        context->profile_actions[index].context = context;
        context->profile_actions[index].profile_index = index;

        const int32_t y = 136 + (int32_t)index * 34;
        context->profile_buttons[index] = create_profile_button(
            page,
            y,
            index,
            wifi_manager_get_sta_profile_ssid(index),
            wifi_manager_get_ap_profile_ssid(index),
            font,
            &context->profile_actions[index]);
        if (context->profile_buttons[index] == NULL) {
            lv_obj_delete(page);
            return NULL;
        }
    }

    return page;
}
