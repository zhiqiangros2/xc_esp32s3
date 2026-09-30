#include "lvgl_ui.h"

#include "lvgl.h"
#include "lvgl_port.h"
#include "lvgl_test_page.h"

/** 点击主界面左上角按钮后，切换到已经创建好的测试页面。 */
static void test_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    /* 创建按钮时通过 user_data 保存了目标页面，点击时直接切换即可。 */
    lv_obj_t *test_page = lv_event_get_user_data(event);
    if (test_page != NULL) {
        lv_screen_load(test_page);
    }
}

esp_err_t lvgl_ui_init(void)
{
    esp_err_t result = lvgl_port_lock();
    if (result != ESP_OK) {
        return result;
    }

    /*
     * 使用 LVGL 初始化时自动创建的活动屏幕作为主界面，并删除旧示例界面的
     * 所有控件，保证主界面只包含下面重新创建的左上角按钮。
     */
    lv_obj_t *main_page = lv_screen_active();
    lv_obj_clean(main_page);

    /* 主界面使用纯白背景和深色文字。 */
    lv_obj_set_style_bg_color(main_page, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(main_page, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(main_page, lv_color_hex(0x17202A), 0);

    /*
     * 测试页面只创建一次，并长期保留。主界面和测试页面之间只切换活动屏幕，
     * 不会在每次点击时重新创建页面，也不会不断占用新的 LVGL 内存。
     */
    lv_obj_t *test_page = lvgl_test_page_create(main_page);
    if (test_page == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }

    /* 在主界面的屏幕坐标 (0, 0) 创建按钮，即 LCD 最左上角。 */
    lv_obj_t *test_button = lv_button_create(main_page);
    lv_obj_set_size(test_button, 112, 44);
    lv_obj_set_pos(test_button, 0, 0);
    lv_obj_set_style_radius(test_button, 4, 0);
    lv_obj_set_style_bg_color(test_button, lv_color_hex(0x1565C0), 0);
    lv_obj_set_style_bg_color(test_button,
                              lv_color_hex(0x0D47A1),
                              LV_STATE_PRESSED);

    /* 将目标页面作为事件参数保存，点击按钮时切换到该页面。 */
    lv_obj_add_event_cb(test_button,
                        test_button_clicked,
                        LV_EVENT_CLICKED,
                        test_page);

    lv_obj_t *button_label = lv_label_create(test_button);
    lv_label_set_text(button_label, "Test Button");
    lv_obj_set_style_text_color(button_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(button_label);

    lvgl_port_unlock();
    return ESP_OK;
}
