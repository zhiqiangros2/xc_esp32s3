#ifndef BOARD_LVGL_UI_H
#define BOARD_LVGL_UI_H

#include "esp_err.h"

/**
 * @brief 创建应用主界面和测试页面。
 *
 * 主界面左上角显示 Test Button。点击后切换到测试页面，测试页面中的
 * Back 按钮可以返回主界面。调用前必须先完成 lvgl_port_init()。
 */
esp_err_t lvgl_ui_init(void);

#endif
