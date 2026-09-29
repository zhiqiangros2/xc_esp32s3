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

/** Fill a rectangular region that lies completely inside the display. */
esp_err_t lcd_fill_rect(uint16_t x,
                        uint16_t y,
                        uint16_t width,
                        uint16_t height,
                        uint16_t color);

/**
 * @brief Display one printable ASCII character.
 *
 * font_height may be 12, 16, 24, or 32 pixels. The character width is half
 * of font_height. (x, y) is the lower-left corner of the character. The
 * glyph height follows the X axis and its width follows the Y axis. Both
 * colors use the RGB565 format.
 */
esp_err_t lcd_show_char(uint16_t x,
                        uint16_t y,
                        char character,
                        uint8_t font_height,
                        uint16_t foreground_color,
                        uint16_t background_color);

/**
 * @brief Display an ASCII string inside a rectangular text area.
 *
 * Characters advance along the positive Y axis. A newline or automatic wrap
 * starts another row along the positive X axis. Drawing stops successfully
 * when no complete row remains. font_height may be 12, 16, 24, or 32 pixels.
 */
esp_err_t lcd_show_string(uint16_t x,
                          uint16_t y,
                          uint16_t x_size,
                          uint16_t y_size,
                          uint8_t font_height,
                          const char *text,
                          uint16_t foreground_color,
                          uint16_t background_color);

/** Draw eight vertical color bars for a quick hardware check. */
esp_err_t lcd_show_test_pattern(void);

#endif
