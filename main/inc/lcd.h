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
 * @brief Initialize the BOX3 2.4-inch ST7789V2 LCD.
 *
 * board_i2c_init() and aw9523b_init() must be called first because the LCD
 * backlight is controlled by AW9523B P1_0. Calling this function more than
 * once is safe. The origin is at the physical lower-left corner: X is the
 * 240-pixel vertical axis and Y is the 320-pixel horizontal axis.
 */
esp_err_t lcd_init(void);

/** Turn the active-low LCD backlight output on or off. */
esp_err_t lcd_backlight_set(bool on);

/** Fill the complete display with one RGB565 color. */
esp_err_t lcd_clear(uint16_t color);

/**
 * Fill a rectangle. height extends along +X; width extends along +Y.
 */
esp_err_t lcd_fill_rect(uint16_t x,
                        uint16_t y,
                        uint16_t height,
                        uint16_t width,
                        uint16_t color);

/**
 * Draw RGB565 pixels. For each Y position, pixels contains height consecutive
 * pixels along the X axis.
 */
esp_err_t lcd_draw_pixels(uint16_t x,
                          uint16_t y,
                          uint16_t height,
                          uint16_t width,
                          const uint16_t *pixels);

#endif
