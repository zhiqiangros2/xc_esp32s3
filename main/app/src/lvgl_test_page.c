#include "lvgl_test_page.h"

#include "lvgl_music_page.h"

static lv_obj_t *s_back_label;
static lv_obj_t *s_music_label;
static lvgl_language_t s_language;
static const lv_font_t *s_ui_font;

/** 页面被删除后清除内部控件指针，避免后续语言切换访问已释放对象。 */
static void test_page_deleted(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        s_back_label = NULL;
        s_music_label = NULL;
        s_ui_font = NULL;
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
    if (s_back_label == NULL) {
        return;
    }

    s_language = language;
    s_ui_font = ui_font;

    const lvgl_language_texts_t *texts = lvgl_language_get_texts(language);
    const lv_font_t *font = LV_FONT_DEFAULT;
    if (language == LVGL_LANGUAGE_ZH_CN && ui_font != NULL) {
        font = ui_font;
    }

    lv_label_set_text(s_back_label, texts->back);
    lv_obj_set_style_text_font(s_back_label, font, 0);
    lv_obj_center(s_back_label);

    if (s_music_label != NULL) {
        lv_label_set_text(s_music_label, texts->music);
        lv_obj_set_style_text_font(s_music_label, font, 0);
        lv_obj_center(s_music_label);
    }
}

lv_obj_t *lvgl_test_page_create(lv_obj_t *main_page,
                                lvgl_language_t language,
                                const lv_font_t *ui_font)
{
    if (main_page == NULL) {
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

    lv_obj_add_event_cb(test_page,
                        test_page_deleted,
                        LV_EVENT_DELETE,
                        NULL);

    /* 测试页面使用浅灰背景和深色文字，与主界面形成直观区别。 */
    lv_obj_set_style_bg_color(test_page, lv_color_hex(0xF2F4F7), 0);
    lv_obj_set_style_bg_opa(test_page, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(test_page, lv_color_hex(0x17202A), 0);

    /* 左上角音乐按钮在点击时创建独立页面。 */
    lv_obj_t *music_button = lv_button_create(test_page);
    lv_obj_set_size(music_button, 92, 44);
    lv_obj_set_pos(music_button, 0, 0);
    lv_obj_set_style_radius(music_button, 4, 0);
    lv_obj_set_style_bg_color(music_button, lv_color_hex(0x00897B), 0);
    lv_obj_set_style_bg_color(music_button,
                              lv_color_hex(0x00695C),
                              LV_STATE_PRESSED);
    lv_obj_add_event_cb(music_button,
                        music_button_clicked,
                        LV_EVENT_CLICKED,
                        NULL);
    s_music_label = lv_label_create(music_button);
    lv_obj_set_style_text_color(s_music_label, lv_color_hex(0xFFFFFF), 0);

    /* 返回按钮紧贴测试页面右下角，文字在按钮内部保持居中。 */
    lv_obj_t *back_button = lv_button_create(test_page);
    lv_obj_set_size(back_button, 76, 44);
    lv_obj_align(back_button, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_radius(back_button, 4, 0);
    lv_obj_set_style_bg_color(back_button, lv_color_hex(0x455A64), 0);
    lv_obj_set_style_bg_color(back_button,
                              lv_color_hex(0x263238),
                              LV_STATE_PRESSED);
    lv_obj_add_event_cb(back_button,
                        back_button_clicked,
                        LV_EVENT_CLICKED,
                        main_page);

    s_back_label = lv_label_create(back_button);
    lv_obj_set_style_text_color(s_back_label, lv_color_hex(0xFFFFFF), 0);
    lvgl_test_page_set_language(language, ui_font);

    return test_page;
}
