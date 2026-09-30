#include "lvgl_test_page.h"

/** 点击测试页面左上角的 Back 按钮后切换回主界面。 */
static void back_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    /* 创建按钮时通过 user_data 保存了需要返回的主界面。 */
    lv_obj_t *main_page = lv_event_get_user_data(event);
    if (main_page != NULL) {
        lv_screen_load(main_page);
    }
}

lv_obj_t *lvgl_test_page_create(lv_obj_t *main_page)
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

    /* 测试页面使用浅灰背景和深色文字，与主界面形成直观区别。 */
    lv_obj_set_style_bg_color(test_page, lv_color_hex(0xF2F4F7), 0);
    lv_obj_set_style_bg_opa(test_page, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(test_page, lv_color_hex(0x17202A), 0);

    /* Back 按钮紧贴测试页面右下角，文字在按钮内部保持居中。 */
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

    lv_obj_t *back_label = lv_label_create(back_button);
    lv_label_set_text(back_label, "Back");
    lv_obj_set_style_text_color(back_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(back_label);

    /* 页面标题居中显示，明确表示当前已经进入新的页面。 */
    lv_obj_t *page_title = lv_label_create(test_page);
    lv_label_set_text(page_title, "New Page");
    lv_obj_align(page_title, LV_ALIGN_CENTER, 0, 0);

    return test_page;
}
