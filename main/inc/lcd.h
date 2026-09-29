#ifndef BOARD_LCD_H
#define BOARD_LCD_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define LCD_X_RESOLUTION 240U
#define LCD_Y_RESOLUTION 320U

#define LCD_COLOR_BLACK 0x0000U   /* 黑色 */
#define LCD_COLOR_BLUE 0x001FU    /* 蓝色 */
#define LCD_COLOR_GREEN 0x07E0U   /* 绿色 */
#define LCD_COLOR_CYAN 0x07FFU    /* 青色 */
#define LCD_COLOR_RED 0xF800U     /* 红色 */
#define LCD_COLOR_MAGENTA 0xF81FU /* 品红色 */
#define LCD_COLOR_YELLOW 0xFFE0U  /* 黄色 */
#define LCD_COLOR_WHITE 0xFFFFU   /* 白色 */
#define LCD_COLOR_LGRAY 0xC618U   /* 浅灰色 */

/**
 * @brief 初始化 BOX3 的 2.4 英寸 ST7789V2 LCD。
 *
 * 调用本函数前必须先完成 board_i2c_init()、aw9523b_init() 和
 * board_spi_init()。LCD 背光由 AW9523B 的 P1_0 控制。本函数允许重复调用。
 * 当前显示坐标原点位于屏幕物理左下角：X 轴是 240 像素的竖直方向，Y 轴是
 * 320 像素的水平方向。
 */
esp_err_t lcd_init(void);

/** 设置 LCD 背光开关。背光输出为低电平有效。 */
esp_err_t lcd_backlight_set(bool on);

/** 使用一种 RGB565 颜色填充整个 LCD 屏幕。 */
esp_err_t lcd_clear(uint16_t color);

/**
 * 填充一个矩形区域。
 * height 表示沿 X 轴方向的长度，width 表示沿 Y 轴方向的长度。
 */
esp_err_t lcd_fill_rect(uint16_t x,
                        uint16_t y,
                        uint16_t height,
                        uint16_t width,
                        uint16_t color);

/**
 * 绘制 RGB565 像素数据。
 * 对于每一个 Y 位置，pixels 中连续保存 height 个沿 X 轴排列的像素。
 */
esp_err_t lcd_draw_pixels(uint16_t x,
                          uint16_t y,
                          uint16_t height,
                          uint16_t width,
                          const uint16_t *pixels);

#endif
