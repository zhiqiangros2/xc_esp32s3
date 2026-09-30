#include "lvgl_ui.h"

#include "lvgl.h"
#include "lvgl_port.h"

static lv_obj_t *hello_label = NULL;

static void hello_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || hello_label == NULL) {
        return;
    }

    /* 点击按钮后修改标签文本；LVGL 会自动标记并刷新发生变化的区域。 */
    lv_label_set_text(hello_label, "Hello World");
}

esp_err_t lvgl_ui_init(void)
{
    esp_err_t result = lvgl_port_lock();
    if (result != ESP_OK) {
        return result;
    }

    lv_obj_t *screen = lv_screen_active();

    /* 使用浅灰背景和深色文字，保持界面在 320x240 屏幕上清晰易读。 */
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xF2F4F7), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0x182230), 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "BOX3 LVGL");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *button = lv_button_create(screen);
    lv_obj_set_size(button, 132, 52);
    lv_obj_align(button, LV_ALIGN_CENTER, 0, -8);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x1677FF), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x0958D9),
                              LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, 6, 0);
    lv_obj_add_event_cb(button,
                        hello_button_clicked,
                        LV_EVENT_CLICKED,
                        NULL);

    lv_obj_t *button_label = lv_label_create(button);
    lv_label_set_text(button_label, "Show message");
    lv_obj_set_style_text_color(button_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(button_label);

    hello_label = lv_label_create(screen);
    lv_label_set_text(hello_label, "");
    /* 固定结果区域，文字从空字符串变为 Hello World 时仍保持水平居中。 */
    lv_obj_set_size(hello_label, 200, 24);
    lv_obj_set_style_text_align(hello_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(hello_label, lv_color_hex(0x1677FF), 0);
    lv_obj_align(hello_label, LV_ALIGN_CENTER, 0, 49);

    lvgl_port_unlock();
    return ESP_OK;
}
