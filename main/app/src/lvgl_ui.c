#include "lvgl_ui.h"

#include "lvgl.h"
#include "lvgl_font.h"
#include "lvgl_language.h"
#include "lvgl_test_page.h"

static lv_font_t *s_ui_font;
static lv_obj_t *s_main_page;
static lv_obj_t *s_button_label;
static bool s_ui_ready;

static void update_main_page_language(lvgl_language_t language)
{
    const lvgl_language_texts_t *texts = lvgl_language_get_texts(language);
    const lv_font_t *font = LV_FONT_DEFAULT;
    if (language == LVGL_LANGUAGE_ZH_CN && s_ui_font != NULL) {
        font = s_ui_font;
    }

    lv_label_set_text(s_button_label, texts->test_item);
    lv_obj_set_style_text_font(s_button_label, font, 0);
    lv_obj_center(s_button_label);
}

/** 每次点击时创建新的测试页面并切换过去。 */
static void test_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    lv_obj_t *test_page = lvgl_test_page_create(s_main_page,
                                                lvgl_language_get(),
                                                s_ui_font);
    if (test_page != NULL) {
        lv_screen_load(test_page);
    }
}

esp_err_t lvgl_ui_init(void)
{
    const esp_err_t font_result = lvgl_font_load_ui(&s_ui_font);
    if (lvgl_language_get() == LVGL_LANGUAGE_ZH_CN &&
        font_result != ESP_OK) {
        (void)lvgl_language_set(LVGL_LANGUAGE_EN);
    }

    /*
     * 使用 LVGL 初始化时自动创建的活动屏幕作为主界面，并删除旧示例界面的
     * 所有控件，保证主界面只包含下面重新创建的左上角按钮。
     */
    s_main_page = lv_screen_active();
    lv_obj_clean(s_main_page);

    /* 主界面使用纯白背景和深色文字。 */
    lv_obj_set_style_bg_color(s_main_page, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_main_page, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_main_page, lv_color_hex(0x17202A), 0);

    /* 在主界面的屏幕坐标 (0, 0) 创建按钮，即 LCD 最左上角。 */
    lv_obj_t *test_button = lv_button_create(s_main_page);
    lv_obj_set_size(test_button, 112, 44);
    lv_obj_set_pos(test_button, 0, 0);
    lv_obj_set_style_radius(test_button, 4, 0);
    lv_obj_set_style_bg_color(test_button, lv_color_hex(0x1565C0), 0);
    lv_obj_set_style_bg_color(test_button,
                              lv_color_hex(0x0D47A1),
                              LV_STATE_PRESSED);

    /* 测试页面在每次点击时创建，并在返回主界面后销毁。 */
    lv_obj_add_event_cb(test_button,
                        test_button_clicked,
                        LV_EVENT_CLICKED,
                        NULL);

    s_button_label = lv_label_create(test_button);
    lv_obj_set_style_text_color(s_button_label, lv_color_hex(0xFFFFFF), 0);
    update_main_page_language(lvgl_language_get());
    s_ui_ready = true;

    /* 完整主界面由专用 LVGL 任务异步刷新。 */
    lv_obj_invalidate(s_main_page);

    return ESP_OK;
}

esp_err_t lvgl_ui_set_language(lvgl_language_t language)
{
    if (language < 0 || language >= LVGL_LANGUAGE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (language == LVGL_LANGUAGE_ZH_CN &&
        s_ui_ready && s_ui_font == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_ui_ready) {
        return lvgl_language_set(language);
    }

    update_main_page_language(language);
    lvgl_test_page_set_language(language, s_ui_font);
    lv_obj_invalidate(s_main_page);
    return lvgl_language_set(language);
}
