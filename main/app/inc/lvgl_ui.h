#ifndef BOARD_LVGL_UI_H
#define BOARD_LVGL_UI_H

#include "esp_err.h"
#include "lvgl_language.h"

/**
 * @brief 创建应用主界面和测试页面。
 *
 * 主界面从 SD 卡 font 目录的 font.ttf 加载 FreeType 字体，并在左上角显示
 * “测试项”。点击时创建测试页面；点击其中的“返回”按钮后销毁测试页面。
 * 调用前必须先完成 SD 卡挂载和 lvgl_port_init()，并且尚未调用
 * lvgl_port_start()。
 */
esp_err_t lvgl_ui_init(void);

/**
 * @brief 切换界面语言并刷新已创建的页面。
 *
 * 中文字体加载失败时不能切换为中文，并返回 ESP_ERR_NOT_SUPPORTED。
 * 界面任务启动后，本函数只能在 LVGL 事件回调中调用。
 */
esp_err_t lvgl_ui_set_language(lvgl_language_t language);

#endif
