#include "display.h"

#include <stdbool.h>
#include <stddef.h>

#include "lcd.h"
#include "lcdfont.h"

typedef struct {
    const unsigned char *bitmap;
    uint8_t width;
    uint8_t height;
    uint8_t bytes_per_row;
} display_ascii_glyph_t;

static bool display_region_is_valid(uint16_t x,
                                    uint16_t y,
                                    uint16_t x_size,
                                    uint16_t y_size)
{
    if (x_size == 0 || y_size == 0) {
        return false;
    }
    if (x >= LCD_X_RESOLUTION || y >= LCD_Y_RESOLUTION) {
        return false;
    }
    if (x_size > LCD_X_RESOLUTION - x ||
        y_size > LCD_Y_RESOLUTION - y) {
        return false;
    }

    return true;
}

static bool display_get_ascii_glyph(char character,
                                    uint8_t font_height,
                                    display_ascii_glyph_t *glyph)
{
    /*
     * char 在部分编译器中默认是有符号类型。先转换为 unsigned char，可避免
     * 字节值大于 0x7F 时被解释成负数，从而保证下面的范围判断结果明确。
     */
    const unsigned char ascii_code = (unsigned char)character;

    /*
     * glyph 是字模信息的输出地址，不能为空。当前字库只包含 95 个可打印
     * ASCII 字符，范围从 0x20（空格）到 0x7E（波浪号）；控制字符、中文
     * 及其他扩展字符均不在该字库中。
     */
    if (glyph == NULL || ascii_code < 0x20U || ascii_code > 0x7EU) {
        return false;
    }

    /*
     * 字库数组从空格开始连续排列。减去 0x20 后，空格对应下标 0，字符
     * '!' 对应下标 1，最后的 '~' 对应下标 94。
     */
    const size_t glyph_index = ascii_code - 0x20U;
    switch (font_height) {
        case 12:
            glyph->bitmap = asc2_1206[glyph_index];
            glyph->width = 6;
            glyph->height = 12;
            glyph->bytes_per_row = 1;
            return true;

        case 16:
            glyph->bitmap = asc2_1608[glyph_index];
            glyph->width = 8;
            glyph->height = 16;
            glyph->bytes_per_row = 1;
            return true;

        case 24:
            glyph->bitmap = asc2_2412[glyph_index];
            glyph->width = 12;
            glyph->height = 24;
            glyph->bytes_per_row = 2;
            return true;

        case 32:
            glyph->bitmap = asc2_3216[glyph_index];
            glyph->width = 16;
            glyph->height = 32;
            glyph->bytes_per_row = 2;
            return true;

        default:
            return false;
    }
}

esp_err_t lcd_show_char(uint16_t x,
                        uint16_t y,
                        char character,
                        uint8_t font_height,
                        uint16_t font_color)
{
    display_ascii_glyph_t glyph;
    /* 检查字符是否属于可打印 ASCII，以及字号是否受当前字库支持。 */
    if (!display_get_ascii_glyph(character, font_height, &glyph)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 检查完整字模是否能够放入以 (x, y) 为起点的屏幕区域。 */
    if (!display_region_is_valid(x, y, glyph.height, glyph.width)) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 字模原始数据按“从上到下逐行、每行从左到右”保存。当前 LCD 的 X 轴
     * 从下向上，Y 轴从左向右，因此显示一个正向字符时需要重新排列像素：
     *
     *  - 字模的行映射到 LCD 的 X 轴，并反转顺序，使字模顶部位于较大的 X；
     *  - 字模的列映射到 LCD 的 Y 轴，保持从左向右的顺序。
     */
    for (uint8_t y_offset = 0; y_offset < glyph.width; ++y_offset) {
        uint8_t x_offset = 0;
        while (x_offset < glyph.height) {
            const uint8_t source_row = glyph.height - 1U - x_offset;
            const uint8_t source_column = y_offset;
            const size_t byte_index =
                (size_t)source_row * glyph.bytes_per_row + source_column / 8U;
            const uint8_t bit_mask =
                (uint8_t)(0x80U >> (source_column % 8U));

            /* 跳过当前行的空白像素，保持屏幕原有背景不变。 */
            if ((glyph.bitmap[byte_index] & bit_mask) == 0) {
                ++x_offset;
                continue;
            }

            /* 找到一段连续笔画，一次写入，减少 LCD 事务次数。 */
            const uint8_t run_start = x_offset;
            ++x_offset;
            while (x_offset < glyph.height) {
                const uint8_t next_source_row = glyph.height - 1U - x_offset;
                const size_t next_byte_index =
                    (size_t)next_source_row * glyph.bytes_per_row +
                    source_column / 8U;
                if ((glyph.bitmap[next_byte_index] & bit_mask) == 0) {
                    break;
                }
                ++x_offset;
            }

            esp_err_t result = lcd_fill_rect((uint16_t)(x + run_start),
                                              (uint16_t)(y + y_offset),
                                              (uint16_t)(x_offset - run_start),
                                              1,
                                              font_color);
            if (result != ESP_OK) {
                return result;
            }
        }
    }

    return ESP_OK;
}

esp_err_t lcd_show_string(uint16_t x,
                          uint16_t y,
                          uint16_t x_size,
                          uint16_t y_size,
                          uint8_t font_height,
                          const char *text,
                          uint16_t font_color)
{
    if (text == NULL || !display_region_is_valid(x, y, x_size, y_size)) {
        return ESP_ERR_INVALID_ARG;
    }

    display_ascii_glyph_t glyph;
    if (!display_get_ascii_glyph(' ', font_height, &glyph)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (x_size < glyph.height || y_size < glyph.width) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 先检查完整字符串，避免绘制一半后才发现不支持的字符。 */
    for (const char *current = text; *current != '\0'; ++current) {
        const unsigned char character = (unsigned char)*current;

        /* 换行符用于切换到下一行，不需要在字库中查找。 */
        if (character == '\n') {
            continue;
        }

        /* 回车符不绘制，排版时会直接忽略。 */
        if (character == '\r') {
            continue;
        }

        /* 0x20 以下是控制字符，当前 ASCII 字库没有对应字模。 */
        if (character < 0x20U) {
            return ESP_ERR_INVALID_ARG;
        }

        /* 0x7E 以上不属于当前字库支持的可打印 ASCII 范围。 */
        if (character > 0x7EU) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    const uint32_t text_area_x_end = (uint32_t)x + x_size;
    const uint32_t text_area_y_end = (uint32_t)y + y_size;
    uint32_t character_x = x;
    uint32_t character_y = y;

    for (const char *current = text; *current != '\0'; ++current) {
        if (*current == '\r') {
            continue;
        }

        if (*current == '\n') {
            character_x += glyph.height;
            character_y = y;
            continue;
        }

        /* Y 轴剩余空间放不下一个完整字符时，沿 X 轴换到下一行。 */
        if (character_y + glyph.width > text_area_y_end) {
            character_x += glyph.height;
            character_y = y;
        }

        /* X 轴剩余空间不足一整行时停止，绝不绘制被截断的字符。 */
        if (character_x + glyph.height > text_area_x_end) {
            break;
        }

        esp_err_t result = lcd_show_char((uint16_t)character_x,
                                         (uint16_t)character_y,
                                         *current,
                                         font_height,
                                         font_color);
        if (result != ESP_OK) {
            return result;
        }

        character_y += glyph.width;
    }

    return ESP_OK;
}

esp_err_t lcd_show_test_pattern(void)
{
    static const uint16_t colors[] = {
        LCD_COLOR_WHITE,
        LCD_COLOR_YELLOW,
        LCD_COLOR_CYAN,
        LCD_COLOR_GREEN,
        LCD_COLOR_MAGENTA,
        LCD_COLOR_RED,
        LCD_COLOR_BLUE,
        LCD_COLOR_BLACK,
    };
    const uint16_t bar_y_size =
        LCD_Y_RESOLUTION / (sizeof(colors) / sizeof(colors[0]));

    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
        esp_err_t result = lcd_fill_rect(0,
                                         (uint16_t)(i * bar_y_size),
                                         LCD_X_RESOLUTION,
                                         bar_y_size,
                                         colors[i]);
        if (result != ESP_OK) {
            return result;
        }
    }

    return ESP_OK;
}
