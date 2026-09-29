#ifndef BOARD_DISPLAY_H
#define BOARD_DISPLAY_H

#include <stdint.h>

#include "esp_err.h"

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
