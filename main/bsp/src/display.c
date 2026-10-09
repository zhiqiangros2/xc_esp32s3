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

/**
 * @brief 读取 PC2LCD2002 逐行 ASCII 字模，并在 LCD 上绘制一个字符。
 *
 * @details 字模从顶部到底部逐行保存，每行从左到右、高位在前。对字符内
 * 坐标 (x_offset, y_offset)，对应字模位的定位方式为：
 *
 *     row_address = bitmap + y_offset * bytes_per_row
 *     byte_index  = x_offset / 8
 *     bit_mask    = 0x80 >> (x_offset % 8)
 *
 * bit_mask 对应位为 1 时绘制 font_color，为 0 时跳过，因此本函数是透明
 * 背景字符绘制，不会清除字符矩形内原有的背景。每一行中相邻的字形像素
 * 会合并成一个水平矩形，再调用一次 lcd_fill_rect()，减少 LCD 传输次数。
 *
 * 字模坐标与 LCD 标准坐标方向相同，不进行旋转或轴交换：
 *
 *     LCD_X = x + x_offset
 *     LCD_Y = y + y_offset
 *
 * 字符矩形与字模偏移的关系如下：
 *
 *                    x_offset 增大，LCD X 向右
 *       (x, y) +--------------------------------+
 *              | (0,0) (1,0) (2,0) ...         |
 *              | (0,1) (1,1) (2,1) ...         | y_offset 增大，
 *              | ...                            | LCD Y 向下
 *              +--------------------------------+
 *              <---- glyph.width 个像素 ------->
 *              共 glyph.height 行
 *
 * 图中左上角就是函数参数 (x, y)。字模中的 (0,0) 写到 LCD 的 (x,y)，
 * 右侧像素增加 LCD X，下面一行增加 LCD Y。
 *
 * @param[in] x 字符左上角的 LCD X 坐标。
 * @param[in] y 字符左上角的 LCD Y 坐标。
 * @param[in] character 可打印 ASCII 字符，范围为 0x20~0x7E。
 * @param[in] font_height 字体高度，只能为 12、16、24 或 32。
 * @param[in] font_color RGB565 字体颜色。
 * @return ESP_OK 绘制成功；参数或完整字符区域无效时返回
 * ESP_ERR_INVALID_ARG；LCD 写入失败时返回 lcd_fill_rect() 的错误码。
 */
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
    if (!display_region_is_valid(x, y, glyph.width, glyph.height)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 字模逐行保存，因此每一行直接对应 LCD 的一个 Y 坐标。 */
    for (uint8_t y_offset = 0; y_offset < glyph.height; ++y_offset) {
        const unsigned char *row_bitmap =
            glyph.bitmap + (size_t)y_offset * glyph.bytes_per_row;
        uint8_t x_offset = 0;

        while (x_offset < glyph.width) {
            /* PC2LCD2002 逐行字模中，bit7 是当前字节最左侧的像素。 */
            const uint8_t byte_index = x_offset / 8U;
            const uint8_t bit_mask =
                (uint8_t)(0x80U >> (x_offset % 8U));

            /* 0 表示空白像素，跳过并保留屏幕原有背景。 */
            if ((row_bitmap[byte_index] & bit_mask) == 0) {
                ++x_offset;
                continue;
            }

            /* 找到一段连续笔画，一次写入，减少 LCD 事务次数。 */
            const uint8_t run_start = x_offset;
            while (x_offset < glyph.width) {
                const uint8_t run_byte_index = x_offset / 8U;
                const uint8_t run_bit_mask =
                    (uint8_t)(0x80U >> (x_offset % 8U));
                if ((row_bitmap[run_byte_index] & run_bit_mask) == 0) {
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

/**
 * @brief 从指定坐标开始，从左到右、从上到下显示 ASCII 字符串。
 *
 * @details 文本区域从 (x, y) 延伸到屏幕右下角。遇到 '\n' 或右侧空间不足
 * 时，从起始 X 坐标换到下一行；'\r' 被忽略。屏幕下方放不下完整字符时
 * 停止并返回 ESP_OK。字符使用透明背景，不会清除未绘制的区域。
 *
 * @param[in] x 文本起始位置的 LCD X 坐标。
 * @param[in] y 文本起始位置的 LCD Y 坐标。
 * @param[in] font_height 字体高度，只能为 12、16、24 或 32。
 * @param[in] text 以 '\0' 结尾的字符串。
 * @param[in] font_color RGB565 字体颜色。
 * @return ESP_OK 显示成功或屏幕已无完整行；参数无效时返回
 * ESP_ERR_INVALID_ARG；字符绘制失败时返回 lcd_show_char() 的错误码。
 */
esp_err_t lcd_show_string(uint16_t x,
                          uint16_t y,
                          uint8_t font_height,
                          const char *text,
                          uint16_t font_color)
{
    if (text == NULL || x >= LCD_X_RESOLUTION || y >= LCD_Y_RESOLUTION) {
        return ESP_ERR_INVALID_ARG;
    }

    display_ascii_glyph_t glyph;
    if (!display_get_ascii_glyph(' ', font_height, &glyph)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (glyph.width > LCD_X_RESOLUTION - x ||
        glyph.height > LCD_Y_RESOLUTION - y) {
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

    const uint32_t text_area_x_end = LCD_X_RESOLUTION;
    const uint32_t text_area_y_end = LCD_Y_RESOLUTION;
    uint32_t character_x = x;
    uint32_t character_y = y;

    for (const char *current = text; *current != '\0'; ++current) {
        if (*current == '\r') {
            continue;
        }

        if (*current == '\n') {
            character_x = x;
            character_y += glyph.height;
            continue;
        }

        /* X 轴剩余空间放不下一个完整字符时，沿 Y 轴换到下一行。 */
        if (character_x + glyph.width > text_area_x_end) {
            character_x = x;
            character_y += glyph.height;
        }

        /* Y 轴剩余空间不足一个完整字符高度时停止。 */
        if (character_y + glyph.height > text_area_y_end) {
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

        character_x += glyph.width;
    }

    return ESP_OK;
}

/**
 * @brief 绘制覆盖整个 320x240 LCD 的八色竖向测试图。
 *
 * @details colors[] 按屏幕从左到右的显示顺序保存颜色。LCD 宽度 320
 * 除以 8 得到每条色带宽度 40；所有色带都从 Y=0 开始，高度为 240，
 * 因此共同覆盖整个屏幕：
 *
 *     X:   0~39  40~79  80~119  120~159  160~199  200~239  240~279  280~319
 *     色:   白      黄      青       绿       品红       红       蓝       黑
 *
 * 屏幕上看到的图案如下：
 *
 *                LCD X 向右，0~319
 *       (0,0) +----+----+----+----+----+----+----+----+
 *             | 白 | 黄 | 青 | 绿 |品红| 红 | 蓝 | 黑 |
 *             |    |    |    |    |    |    |    |    |
 *             |    |    |    |    |    |    |    |    |
 *      (0,239)+----+----+----+----+----+----+----+----+
 *             |
 *             v LCD Y 向下，0~239
 *
 * 每次调用 lcd_fill_rect() 绘制一条完整色带。如果某一次 LCD 传输失败，
 * 函数立即返回该错误，不再继续绘制右侧剩余色带。
 *
 * @return ESP_OK 绘制成功；否则返回 lcd_fill_rect() 的错误码。
 */
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
    const uint16_t bar_width =
        LCD_X_RESOLUTION / (sizeof(colors) / sizeof(colors[0]));

    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
        esp_err_t result = lcd_fill_rect((uint16_t)(i * bar_width),
                                         0,
                                         bar_width,
                                         LCD_Y_RESOLUTION,
                                         colors[i]);
        if (result != ESP_OK) {
            return result;
        }
    }

    return ESP_OK;
}
