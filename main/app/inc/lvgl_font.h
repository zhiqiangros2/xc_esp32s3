#ifndef BOARD_LVGL_FONT_H
#define BOARD_LVGL_FONT_H

#include "esp_err.h"
#include "lvgl.h"

#define LVGL_UI_FONT_PATH "/sdcard/font/font.ttf"

/**
 * @brief 从 SD 卡加载支持多语言 UTF-8 字符的界面 TTF 字体。
 *
 * 调用前必须先挂载 SD 卡并初始化 LVGL，且调用方必须持有 LVGL 锁。
 * 字体对象由本模块长期持有，调用方不得释放。
 *
 * @param[out] font 返回加载成功的 LVGL 字体对象。
 */
esp_err_t lvgl_font_load_ui(lv_font_t **font);

#endif
