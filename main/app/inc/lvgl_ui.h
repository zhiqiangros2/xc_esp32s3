#ifndef BOARD_LVGL_UI_H
#define BOARD_LVGL_UI_H

#include "esp_err.h"

/**
 * @brief 创建 BOX3 的 LVGL 示例界面。
 *
 * 界面包含一个按钮和一个结果标签。点击按钮后，结果标签显示
 * "Hello World"。调用前必须先完成 lvgl_port_init()。
 */
esp_err_t lvgl_ui_init(void);

#endif
