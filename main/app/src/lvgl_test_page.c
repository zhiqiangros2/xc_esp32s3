#include "lvgl_test_page.h"

#include "lvgl_bluetooth_page.h"
#include "lvgl_camera_page.h"
#include "lvgl_music_page.h"
#include "lvgl_record_page.h"
#include "lvgl_wav_manager_page.h"
#include "lvgl_wifi_page.h"

#include <stdbool.h>

#include "esp_log.h"

/* 间隔略大于屏幕刷新周期，保证两个新控件之间至少完成一次绘制。 */
#define TEST_PAGE_CONTROL_CREATE_INTERVAL_MS (LV_DEF_REFR_PERIOD + 1U)

typedef enum {
    TEST_PAGE_CONTROL_MUSIC,
    TEST_PAGE_CONTROL_RECORD,
    TEST_PAGE_CONTROL_WAV_MANAGER,
    TEST_PAGE_CONTROL_CAMERA,
    TEST_PAGE_CONTROL_BLUETOOTH,
    TEST_PAGE_CONTROL_WIFI,
    TEST_PAGE_CONTROL_BACK,
    TEST_PAGE_CONTROL_COMPLETE,
} test_page_control_t;

static lv_obj_t *s_test_page;
static lv_obj_t *s_main_page;
static lv_obj_t *s_back_label;
static lv_obj_t *s_music_label;
static lv_obj_t *s_record_label;
static lv_obj_t *s_wav_manager_label;
static lv_obj_t *s_camera_label;
static lv_obj_t *s_bluetooth_label;
static lv_obj_t *s_wifi_label;
static lv_timer_t *s_build_timer;
static lvgl_language_t s_language;
static const lv_font_t *s_ui_font;
static test_page_control_t s_next_control;
static bool s_build_started;
static const char *TAG = "TEST_PAGE";

static void create_next_control(lv_timer_t *timer);

/** 页面被删除后清除内部控件指针，避免后续语言切换访问已释放对象。 */
static void test_page_deleted(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        if (s_build_timer != NULL) {
            lv_timer_delete(s_build_timer);
            s_build_timer = NULL;
        }
        s_test_page = NULL;
        s_main_page = NULL;
        s_back_label = NULL;
        s_music_label = NULL;
        s_record_label = NULL;
        s_wav_manager_label = NULL;
        s_camera_label = NULL;
        s_bluetooth_label = NULL;
        s_wifi_label = NULL;
        s_ui_font = NULL;
        s_next_control = TEST_PAGE_CONTROL_COMPLETE;
        s_build_started = false;
    }
}

/** 点击时创建 WiFi 页面；关闭页面不影响 STA、SoftAP 或自动重连。 */
static void wifi_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    lv_obj_t *test_page = lv_obj_get_screen(lv_event_get_target(event));
    lv_obj_t *wifi_page = lvgl_wifi_page_create(test_page,
                                                s_language,
                                                s_ui_font);
    if (wifi_page != NULL) {
        lv_screen_load(wifi_page);
    }
}

/** 点击时创建 BLE 设备页面；页面只发起一次有限时长的浏览扫描。 */
static void bluetooth_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    lv_obj_t *test_page = lv_obj_get_screen(lv_event_get_target(event));
    lv_obj_t *bluetooth_page = lvgl_bluetooth_page_create(test_page,
                                                          s_language,
                                                          s_ui_font);
    if (bluetooth_page != NULL) {
        lv_screen_load(bluetooth_page);
    }
}

/** 首帧只绘制页面背景；完成后启动逐个控件创建定时器。 */
static void test_page_drawn(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DRAW_POST || s_build_started) {
        return;
    }

    s_build_started = true;
    s_build_timer = lv_timer_create(create_next_control,
                                     TEST_PAGE_CONTROL_CREATE_INTERVAL_MS,
                                     NULL);
    if (s_build_timer == NULL) {
        ESP_LOGE(TAG, "Cannot create test page build timer");
    }
}

/** 点击时创建摄像头预览页面；关闭后返回并销毁预览页。 */
static void camera_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    lv_obj_t *test_page = lv_obj_get_screen(lv_event_get_target(event));
    lv_obj_t *camera_page = lvgl_camera_page_create(test_page,
                                                    s_language,
                                                    s_ui_font);
    if (camera_page == NULL) {
        ESP_LOGE(TAG, "Camera page creation failed");
        return;
    }

    lv_screen_load(camera_page);
}

/** 点击时创建 WAV 管理页面；页面显示后由该页面增量扫描目录。 */
static void wav_manager_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    lv_obj_t *test_page = lv_obj_get_screen(lv_event_get_target(event));
    lv_obj_t *manager_page = lvgl_wav_manager_page_create(test_page,
                                                          s_language,
                                                          s_ui_font);
    if (manager_page != NULL) {
        lv_screen_load(manager_page);
    }
}

/** 点击时创建录音页面；关闭后返回测试页并销毁录音页面。 */
static void record_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    lv_obj_t *test_page = lv_obj_get_screen(lv_event_get_target(event));
    lv_obj_t *record_page = lvgl_record_page_create(test_page,
                                                    s_language,
                                                    s_ui_font);
    if (record_page != NULL) {
        lv_screen_load(record_page);
    }
}

/** 点击时创建音乐页面；关闭音乐页面后会回到当前测试页面。 */
static void music_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    lv_obj_t *test_page = lv_obj_get_screen(lv_event_get_target(event));
    lv_obj_t *music_page = lvgl_music_page_create(test_page,
                                                  s_language,
                                                  s_ui_font);
    if (music_page != NULL) {
        lv_screen_load(music_page);
    }
}

/** 返回主界面，并在当前事件处理完成后销毁测试页面。 */
static void back_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    /* 创建按钮时通过 user_data 保存了需要返回的主界面。 */
    lv_obj_t *main_page = lv_event_get_user_data(event);
    if (main_page != NULL) {
        lv_obj_t *test_page = lv_obj_get_screen(lv_event_get_target(event));
        lv_screen_load(main_page);
        lv_obj_delete_async(test_page);
    }
}

void lvgl_test_page_set_language(lvgl_language_t language,
                                 const lv_font_t *ui_font)
{
    s_language = language;
    s_ui_font = ui_font;

    const lvgl_language_texts_t *texts = lvgl_language_get_texts(language);
    const lv_font_t *font = LV_FONT_DEFAULT;
    if (language == LVGL_LANGUAGE_ZH_CN && ui_font != NULL) {
        font = ui_font;
    }

    if (s_back_label != NULL) {
        lv_label_set_text(s_back_label, texts->back);
        lv_obj_set_style_text_font(s_back_label, font, 0);
        lv_obj_center(s_back_label);
    }

    if (s_music_label != NULL) {
        lv_label_set_text(s_music_label, texts->music);
        lv_obj_set_style_text_font(s_music_label, font, 0);
        lv_obj_center(s_music_label);
    }
    if (s_record_label != NULL) {
        lv_label_set_text(s_record_label, texts->record);
        lv_obj_set_style_text_font(s_record_label, font, 0);
        lv_obj_center(s_record_label);
    }
    if (s_wav_manager_label != NULL) {
        lv_label_set_text(s_wav_manager_label, texts->wav_manager);
        lv_obj_set_style_text_font(s_wav_manager_label, font, 0);
        lv_obj_center(s_wav_manager_label);
    }
    if (s_camera_label != NULL) {
        lv_label_set_text(s_camera_label, texts->camera);
        lv_obj_set_style_text_font(s_camera_label, font, 0);
        lv_obj_center(s_camera_label);
    }
    if (s_bluetooth_label != NULL) {
        lv_label_set_text(s_bluetooth_label, texts->bluetooth);
        lv_obj_set_style_text_font(s_bluetooth_label, font, 0);
        lv_obj_center(s_bluetooth_label);
    }
    if (s_wifi_label != NULL) {
        lv_label_set_text(s_wifi_label, texts->wifi);
        lv_obj_set_style_text_font(s_wifi_label, font, 0);
        lv_obj_center(s_wifi_label);
    }
}

/**
 * 每次定时器回调只创建一个按钮及其标签。
 *
 * 中文 TTF 的新字形会在标签首次绘制时按需读取和生成。把七组控件拆到不同
 * LVGL 周期，可以让触摸、刷新和其他任务在两组控件之间继续运行，避免测试页
 * 首次打开时长时间停留在旧页面。
 */
static void create_next_control(lv_timer_t *timer)
{
    if (s_test_page == NULL) {
        lv_timer_delete(timer);
        s_build_timer = NULL;
        return;
    }

    const lvgl_language_texts_t *texts = lvgl_language_get_texts(s_language);
    const lv_font_t *font = LV_FONT_DEFAULT;
    if (s_language == LVGL_LANGUAGE_ZH_CN && s_ui_font != NULL) {
        font = s_ui_font;
    }

    lv_obj_t *button;
    switch (s_next_control) {
        case TEST_PAGE_CONTROL_MUSIC:
            button = lv_button_create(s_test_page);
            lv_obj_set_size(button, 92, 44);
            lv_obj_set_pos(button, 0, 0);
            lv_obj_set_style_radius(button, 4, 0);
            lv_obj_set_style_bg_color(button, lv_color_hex(0x00897B), 0);
            lv_obj_set_style_bg_color(button,
                                      lv_color_hex(0x00695C),
                                      LV_STATE_PRESSED);
            lv_obj_add_event_cb(button,
                                music_button_clicked,
                                LV_EVENT_CLICKED,
                                NULL);
            s_music_label = lv_label_create(button);
            lv_label_set_text(s_music_label, texts->music);
            lv_obj_set_style_text_font(s_music_label, font, 0);
            lv_obj_set_style_text_color(s_music_label,
                                        lv_color_hex(0xFFFFFF),
                                        0);
            lv_obj_center(s_music_label);
            s_next_control = TEST_PAGE_CONTROL_RECORD;
            break;

        case TEST_PAGE_CONTROL_RECORD:
            button = lv_button_create(s_test_page);
            lv_obj_set_size(button, 92, 44);
            lv_obj_set_pos(button, 100, 0);
            lv_obj_set_style_radius(button, 4, 0);
            lv_obj_set_style_bg_color(button, lv_color_hex(0xC62828), 0);
            lv_obj_set_style_bg_color(button,
                                      lv_color_hex(0x8E0000),
                                      LV_STATE_PRESSED);
            lv_obj_add_event_cb(button,
                                record_button_clicked,
                                LV_EVENT_CLICKED,
                                NULL);
            s_record_label = lv_label_create(button);
            lv_label_set_text(s_record_label, texts->record);
            lv_obj_set_style_text_font(s_record_label, font, 0);
            lv_obj_set_style_text_color(s_record_label,
                                        lv_color_hex(0xFFFFFF),
                                        0);
            lv_obj_center(s_record_label);
            s_next_control = TEST_PAGE_CONTROL_WAV_MANAGER;
            break;

        case TEST_PAGE_CONTROL_WAV_MANAGER:
            button = lv_button_create(s_test_page);
            lv_obj_set_size(button, 112, 44);
            lv_obj_set_pos(button, 200, 0);
            lv_obj_set_style_radius(button, 4, 0);
            lv_obj_set_style_bg_color(button, lv_color_hex(0x1565C0), 0);
            lv_obj_set_style_bg_color(button,
                                      lv_color_hex(0x0D47A1),
                                      LV_STATE_PRESSED);
            lv_obj_add_event_cb(button,
                                wav_manager_button_clicked,
                                LV_EVENT_CLICKED,
                                NULL);
            s_wav_manager_label = lv_label_create(button);
            lv_label_set_text(s_wav_manager_label, texts->wav_manager);
            lv_obj_set_style_text_font(s_wav_manager_label, font, 0);
            lv_obj_set_style_text_color(s_wav_manager_label,
                                        lv_color_hex(0xFFFFFF),
                                        0);
            lv_obj_center(s_wav_manager_label);
            s_next_control = TEST_PAGE_CONTROL_CAMERA;
            break;

        case TEST_PAGE_CONTROL_CAMERA:
            button = lv_button_create(s_test_page);
            lv_obj_set_size(button, 92, 44);
            lv_obj_set_pos(button, 0, 52);
            lv_obj_set_style_radius(button, 4, 0);
            lv_obj_set_style_bg_color(button, lv_color_hex(0x5D4037), 0);
            lv_obj_set_style_bg_color(button,
                                      lv_color_hex(0x3E2723),
                                      LV_STATE_PRESSED);
            lv_obj_add_event_cb(button,
                                camera_button_clicked,
                                LV_EVENT_CLICKED,
                                NULL);
            s_camera_label = lv_label_create(button);
            lv_label_set_text(s_camera_label, texts->camera);
            lv_obj_set_style_text_font(s_camera_label, font, 0);
            lv_obj_set_style_text_color(s_camera_label,
                                        lv_color_hex(0xFFFFFF),
                                        0);
            lv_obj_center(s_camera_label);
            s_next_control = TEST_PAGE_CONTROL_BLUETOOTH;
            break;

        case TEST_PAGE_CONTROL_BLUETOOTH:
            button = lv_button_create(s_test_page);
            lv_obj_set_size(button, 112, 44);
            lv_obj_set_pos(button, 100, 52);
            lv_obj_set_style_radius(button, 4, 0);
            lv_obj_set_style_bg_color(button, lv_color_hex(0x00695C), 0);
            lv_obj_set_style_bg_color(button,
                                      lv_color_hex(0x004D40),
                                      LV_STATE_PRESSED);
            lv_obj_add_event_cb(button,
                                bluetooth_button_clicked,
                                LV_EVENT_CLICKED,
                                NULL);
            s_bluetooth_label = lv_label_create(button);
            lv_label_set_text(s_bluetooth_label, texts->bluetooth);
            lv_obj_set_style_text_font(s_bluetooth_label, font, 0);
            lv_obj_set_style_text_color(s_bluetooth_label,
                                        lv_color_hex(0xFFFFFF),
                                        0);
            lv_obj_center(s_bluetooth_label);
            s_next_control = TEST_PAGE_CONTROL_WIFI;
            break;

        case TEST_PAGE_CONTROL_WIFI:
            button = lv_button_create(s_test_page);
            lv_obj_set_size(button, 92, 44);
            lv_obj_set_pos(button, 220, 52);
            lv_obj_set_style_radius(button, 4, 0);
            lv_obj_set_style_bg_color(button, lv_color_hex(0x37474F), 0);
            lv_obj_set_style_bg_color(button,
                                      lv_color_hex(0x263238),
                                      LV_STATE_PRESSED);
            lv_obj_add_event_cb(button,
                                wifi_button_clicked,
                                LV_EVENT_CLICKED,
                                NULL);
            s_wifi_label = lv_label_create(button);
            lv_label_set_text(s_wifi_label, texts->wifi);
            lv_obj_set_style_text_font(s_wifi_label, font, 0);
            lv_obj_set_style_text_color(s_wifi_label,
                                        lv_color_hex(0xFFFFFF),
                                        0);
            lv_obj_center(s_wifi_label);
            s_next_control = TEST_PAGE_CONTROL_BACK;
            break;

        case TEST_PAGE_CONTROL_BACK:
            button = lv_button_create(s_test_page);
            lv_obj_set_size(button, 76, 44);
            lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
            lv_obj_set_style_radius(button, 4, 0);
            lv_obj_set_style_bg_color(button, lv_color_hex(0x455A64), 0);
            lv_obj_set_style_bg_color(button,
                                      lv_color_hex(0x263238),
                                      LV_STATE_PRESSED);
            lv_obj_add_event_cb(button,
                                back_button_clicked,
                                LV_EVENT_CLICKED,
                                s_main_page);
            s_back_label = lv_label_create(button);
            lv_label_set_text(s_back_label, texts->back);
            lv_obj_set_style_text_font(s_back_label, font, 0);
            lv_obj_set_style_text_color(s_back_label,
                                        lv_color_hex(0xFFFFFF),
                                        0);
            lv_obj_center(s_back_label);
            s_next_control = TEST_PAGE_CONTROL_COMPLETE;
            break;

        case TEST_PAGE_CONTROL_COMPLETE:
            break;
    }

    if (s_next_control == TEST_PAGE_CONTROL_COMPLETE) {
        lv_timer_delete(timer);
        s_build_timer = NULL;
    }
}

lv_obj_t *lvgl_test_page_create(lv_obj_t *main_page,
                                lvgl_language_t language,
                                const lv_font_t *ui_font)
{
    if (main_page == NULL || s_test_page != NULL) {
        return NULL;
    }

    /*
     * parent 传入 NULL 表示创建一个独立屏幕，而不是在当前主界面内创建普通
     * 子控件。只有独立屏幕才能通过 lv_screen_load() 完整切换页面。
     */
    lv_obj_t *test_page = lv_obj_create(NULL);
    if (test_page == NULL) {
        return NULL;
    }

    s_test_page = test_page;
    s_main_page = main_page;
    s_language = language;
    s_ui_font = ui_font;
    s_next_control = TEST_PAGE_CONTROL_MUSIC;
    s_build_started = false;

    lv_obj_add_event_cb(test_page,
                        test_page_deleted,
                        LV_EVENT_DELETE,
                        NULL);
    lv_obj_add_event_cb(test_page,
                        test_page_drawn,
                        LV_EVENT_DRAW_POST,
                        NULL);

    /* 测试页面使用浅灰背景和深色文字，与主界面形成直观区别。 */
    lv_obj_set_style_bg_color(test_page, lv_color_hex(0xF2F4F7), 0);
    lv_obj_set_style_bg_opa(test_page, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(test_page, lv_color_hex(0x17202A), 0);

    /*
     * 创建函数到这里立即返回。调用者加载屏幕后先得到完整背景，DRAW_POST
     * 回调随后启动定时器，按音乐、录音、WAV 管理、摄像头、蓝牙、WiFi、返回的顺序
     * 逐个创建控件，避免所有中文 TTF 字形集中在同一个首帧内生成。
     */
    return test_page;
}
