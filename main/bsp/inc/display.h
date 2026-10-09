#ifndef BOARD_DISPLAY_H
#define BOARD_DISPLAY_H

#include <stdint.h>

#include "esp_err.h"

/**
 * @brief 在标准 LCD 坐标中显示一个可打印 ASCII 字符。
 *
 * @details 当前字库支持 ASCII 0x20~0x7E 和 12、16、24、32 四种字体高度，
 * 对应字符尺寸分别为 6x12、8x16、12x24、16x32 像素。这里的尺寸顺序为
 * “宽度 x 高度”。
 *
 * (x, y) 是字符左上角，X 轴从左向右，Y 轴从上向下。函数只用
 * font_color 绘制字模中值为 1 的笔画像素；值为 0 的空白像素不会写入
 * LCD，因此字符背景保持原来的屏幕内容。
 *
 * 字符在屏幕中的位置如下。字符宽度等于 font_height / 2，字符高度等于
 * font_height：
 *
 *                  X 轴向右
 *       (x, y) +-------------------+
 *              |                   |
 *              |     character     |  字符高度 = font_height
 *              |                   |
 *              +-------------------+
 *                字符宽度 = font_height / 2
 *              |
 *              v Y 轴向下
 *
 * (x, y) 必须保证上面的完整字符矩形都位于 320x240 屏幕内。
 *
 * @param[in] x 字符左上角的水平坐标，范围为 0~319。
 * @param[in] y 字符左上角的竖直坐标，范围为 0~239。
 * @param[in] character 要显示的可打印 ASCII 字符，范围为 0x20~0x7E。
 * @param[in] font_height 字体高度，只能是 12、16、24 或 32。
 * @param[in] font_color 字体颜色，格式为 RGB565。
 * @return ESP_OK 显示成功；字符、字号或显示区域无效时返回
 * ESP_ERR_INVALID_ARG；LCD 传输失败时返回对应的底层错误。
 */
esp_err_t lcd_show_char(uint16_t x,
                        uint16_t y,
                        char character,
                        uint8_t font_height,
                        uint16_t font_color);

/**
 * @brief 从指定坐标开始显示 ASCII 字符串。
 *
 * @details 文本区域从 (x, y) 延伸到屏幕右下角。字符从左向右排列；遇到
 * '\n' 或右侧空间不足时，从起始 X 坐标绘制下一行；'\r' 被忽略。屏幕
 * 下方放不下完整字符时正常停止，不绘制被截断的字符。
 *
 * font_height 可以是 12、16、24 或 32，对应字符宽度分别为 6、8、12、
 * 16 像素。字符使用透明背景；更新可变字符串时，如需清除旧内容，应先
 * 调用 lcd_fill_rect()。
 *
 * @param[in] x 文本起始位置的水平坐标，范围为 0~319。
 * @param[in] y 文本起始位置的竖直坐标，范围为 0~239。
 * @param[in] font_height 字体高度，只能是 12、16、24 或 32。
 * @param[in] text 以 '\0' 结尾的 ASCII 字符串，不能为 NULL。
 * @param[in] font_color 字体颜色，格式为 RGB565。
 * @return ESP_OK 排版和绘制成功；坐标、字号或字符无效时返回
 * ESP_ERR_INVALID_ARG；LCD 绘制失败时返回 lcd_show_char() 的错误码。
 */
esp_err_t lcd_show_string(uint16_t x,
                          uint16_t y,
                          uint8_t font_height,
                          const char *text,
                          uint16_t font_color);

/**
 * @brief 在整个 LCD 上显示八条竖向 RGB565 测试色带。
 *
 * @details 屏幕宽度为 320 像素，函数将其平均分为 8 条，每条宽 40 像素、
 * 高 240 像素。色带按照 LCD X 轴从左向右依次为：白、黄、青、绿、品红、
 * 红、蓝、黑。
 *
 * 实际显示图案如下，每个方框表示一条 40x240 的竖向色带：
 *
 *                X 轴向右，范围 0~319
 *       (0,0) +----+----+----+----+----+----+----+----+
 *             | 白 | 黄 | 青 | 绿 |品红| 红 | 蓝 | 黑 |
 *             |    |    |    |    |    |    |    |    |
 *             |    |    |    |    |    |    |    |    |
 *      (0,239)+----+----+----+----+----+----+----+----+
 *             |
 *             v Y 轴向下，范围 0~239
 *
 * 该图案可用于检查：
 *
 *  - LCD 是否能够覆盖刷新整个 320x240 显示区域；
 *  - X 轴是否从左向右递增，Y 轴是否从上向下覆盖完整屏幕；
 *  - RGB565 的红、绿、蓝分量和字节顺序是否正确；
 *  - 相邻矩形区域之间是否存在漏画、错位或异常间隙。
 *
 * @return ESP_OK 八条色带全部绘制成功；任意色带绘制失败时立即停止，
 * 并返回 lcd_fill_rect() 对应的错误码。
 */
esp_err_t lcd_show_test_pattern(void);

#endif
