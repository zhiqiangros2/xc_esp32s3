#ifndef BOARD_LCD_H
#define BOARD_LCD_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define LCD_X_RESOLUTION 320U
#define LCD_Y_RESOLUTION 240U

#define LCD_COLOR_BLACK 0x0000U   /* 黑色 */
#define LCD_COLOR_BLUE 0x001FU    /* 蓝色 */
#define LCD_COLOR_GREEN 0x07E0U   /* 绿色 */
#define LCD_COLOR_CYAN 0x07FFU    /* 青色 */
#define LCD_COLOR_RED 0xF800U     /* 红色 */
#define LCD_COLOR_MAGENTA 0xF81FU /* 品红色 */
#define LCD_COLOR_YELLOW 0xFFE0U  /* 黄色 */
#define LCD_COLOR_WHITE 0xFFFFU   /* 白色 */
#define LCD_COLOR_LGRAY 0xC618U   /* 浅灰色 */

/** LCD 异步颜色传输完成回调，在 SPI DMA 完成中断中执行。 */
typedef bool (*lcd_transfer_done_callback_t)(void *user_context);

/**
 * @brief 初始化 BOX3 的 2.4 英寸 ST7789V2 LCD。
 *
 * 调用本函数前必须先完成 board_i2c_init()、aw9523b_init() 和
 * board_spi_init()。LCD 背光由 AW9523B 的 P1_0 控制。本函数允许重复调用。
 * 显示坐标与 LVGL 一致：原点位于左上角，X 轴沿水平方向向右递增，范围为
 * 0~319；Y 轴沿竖直方向向下递增，范围为 0~239。
 */
esp_err_t lcd_init(void);

/** 设置 LCD 背光开关。背光输出为低电平有效。 */
esp_err_t lcd_backlight_set(bool on);

/** 使用一种 RGB565 颜色填充整个 LCD 屏幕。 */
esp_err_t lcd_clear(uint16_t color);

/**
 * @brief 使用一种 RGB565 颜色填充矩形区域。
 *
 * (x, y) 是矩形左上角。width 沿 X 轴向右延伸，height 沿 Y 轴向下延伸。
 */
esp_err_t lcd_fill_rect(uint16_t x,
                        uint16_t y,
                        uint16_t width,
                        uint16_t height,
                        uint16_t color);

/**
 * @brief 按标准从左到右、从上到下的顺序绘制 RGB565 像素数据。
 *
 * @details (x, y) 是目标矩形的左上角，width 是矩形宽度，height 是矩形
 * 高度。pixels 必须至少包含 width * height 个 uint16_t 像素，排列方式为：
 *
 *     pixels[0]                 -> LCD (x,             y)
 *     pixels[1]                 -> LCD (x + 1,         y)
 *     pixels[width - 1]         -> LCD (x + width - 1, y)
 *     pixels[width]             -> LCD (x,             y + 1)
 *     pixels[row * width + col] -> LCD (x + col,       y + row)
 *
 * 可以直观看成下面的矩形：
 *
 *                         LCD X 向右
 *       (x, y) +----------------------------------+
 *              | pixels[0] ... pixels[width - 1] |  第 0 行
 *              | pixels[width] ...               |  第 1 行
 *              | ...                             |
 *              +----------------------------------+
 *              <------------- width ------------->
 *              |
 *              | height
 *              v LCD Y 向下
 *
 * 该排列与 LVGL 的 RGB565 显示缓冲区一致，不需要旋转或交换 X/Y。
 * 矩形内每个像素都会被 pixels 中对应的颜色覆盖，没有透明背景。函数内部
 * 按 DMA 缓冲区容量分批发送，并等待全部 SPI DMA 传输完成后才返回；返回
 * 后调用者可以立即修改或释放 pixels。
 *
 * @param[in] x 目标矩形左上角的 X 坐标，范围为 0~319。
 * @param[in] y 目标矩形左上角的 Y 坐标，范围为 0~239。
 * @param[in] width 目标矩形宽度，沿 X 轴向右延伸，不能为 0。
 * @param[in] height 目标矩形高度，沿 Y 轴向下延伸，不能为 0。
 * @param[in] pixels RGB565 像素数组，不能为 NULL。
 * @return ESP_OK 绘制成功；LCD 未初始化时返回 ESP_ERR_INVALID_STATE；
 * 参数或矩形范围无效时返回 ESP_ERR_INVALID_ARG；SPI/LCD 传输失败时返回
 * 对应的底层错误码。
 */
esp_err_t lcd_draw_pixels(uint16_t x,
                          uint16_t y,
                          uint16_t width,
                          uint16_t height,
                          const uint16_t *pixels);

/**
 * @brief 异步发送已经按 LCD 字节序排列的 DMA 像素缓冲区。
 *
 * pixels 必须位于 DMA 可访问内存，并在 done_callback 被调用前保持不变。
 * 大于 SPI2 单次传输上限的数据由 ESP-IDF 自动拆分；函数成功返回仅表示全部
 * 分段已经提交，最后一段 DMA 完成后在中断上下文调用 done_callback。
 */
esp_err_t lcd_draw_rgb565_bytes_async(
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height,
    const uint8_t *pixels,
    lcd_transfer_done_callback_t done_callback,
    void *user_context);

/**
 * @brief 绘制已经按 LCD 传输字节序排列的 RGB565 图像。
 *
 * @details 本接口供 GC0308 等直接输出 RGB565 字节流的设备使用。pixels
 * 必须按从左到右、从上到下的顺序保存，每个像素占两个连续字节，并且先放
 * RGB565 高 8 位，再放低 8 位：
 *
 *     pixels[0] = 第 0 个像素的 RGB565 高字节
 *     pixels[1] = 第 0 个像素的 RGB565 低字节
 *     pixels[2] = 第 1 个像素的 RGB565 高字节
 *     pixels[3] = 第 1 个像素的 RGB565 低字节
 *
 * 坐标和矩形方向与 lcd_draw_pixels() 相同：(x, y) 是左上角，width 沿
 * X 轴向右，height 沿 Y 轴向下。函数把源数据分批复制到片内 DMA 缓冲区，
 * 等待每批 SPI 传输完成后再处理下一批；返回后调用者可以立即复用 pixels。
 *
 * 与 lcd_draw_pixels() 的区别是：lcd_draw_pixels() 接收 ESP32 内存中的
 * uint16_t RGB565 数值，并会交换每个像素的高低字节；本函数接收已经是
 * “高字节、低字节”顺序的原始字节流，不再执行字节交换。
 *
 * @param[in] x 目标矩形左上角的 X 坐标。
 * @param[in] y 目标矩形左上角的 Y 坐标。
 * @param[in] width 目标矩形宽度，不能为 0。
 * @param[in] height 目标矩形高度，不能为 0。
 * @param[in] pixels 至少包含 width * height * 2 字节的 RGB565 数据。
 * @return ESP_OK 绘制成功；LCD 未初始化、参数无效或底层传输失败时返回
 * 对应错误码。
 */
esp_err_t lcd_draw_rgb565_bytes(uint16_t x,
                                uint16_t y,
                                uint16_t width,
                                uint16_t height,
                                const uint8_t *pixels);

#endif
