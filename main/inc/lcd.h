#ifndef BOARD_LCD_H
#define BOARD_LCD_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define LCD_WIDTH_320 320U
#define LCD_HEIGHT_240 240U

#define LCD_COLOR_BLACK 0x0000U
#define LCD_COLOR_BLUE 0x001FU
#define LCD_COLOR_GREEN 0x07E0U
#define LCD_COLOR_CYAN 0x07FFU
#define LCD_COLOR_RED 0xF800U
#define LCD_COLOR_MAGENTA 0xF81FU
#define LCD_COLOR_YELLOW 0xFFE0U
#define LCD_COLOR_WHITE 0xFFFFU

/**
 * @brief Initialize the BOX3 2.4-inch ST7789V2 LCD in landscape mode.
 *
 * board_i2c_init() and aw9523b_init() must be called first because the LCD
 * backlight is controlled by AW9523B P1_0. Calling this function more than
 * once is safe.
 */
esp_err_t lcd_init(void);

/** Turn the active-low LCD backlight output on or off. */
esp_err_t lcd_backlight_set(bool on);

/** Fill the complete display with one RGB565 color. */
esp_err_t lcd_clear(uint16_t color);

/** Fill a rectangular region that lies completely inside the display. */
esp_err_t lcd_fill_rect(uint16_t x,
                        uint16_t y,
                        uint16_t width,
                        uint16_t height,
                        uint16_t color);

/**
 * Draw a row-major RGB565 bitmap. Pixel values use CPU-native uint16_t byte
 * order; the driver converts them to the ST7789 wire byte order.
 */
esp_err_t lcd_draw_bitmap(uint16_t x,
                          uint16_t y,
                          uint16_t width,
                          uint16_t height,
                          const uint16_t *pixels);

/** Draw eight vertical color bars for a quick hardware check. */
esp_err_t lcd_show_test_pattern(void);

#endif
