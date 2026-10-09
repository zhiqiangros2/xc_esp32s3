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

/**
 * @brief LCD 异步颜色传输完成回调。
 *
 * @param[in] user_context 调用异步绘制接口时传入的用户上下文指针。
 * @return 返回 true 表示回调唤醒了更高优先级任务，需要在退出中断时切换任务；
 * 返回 false 表示不需要切换任务。
 *
 * @warning 回调在 SPI DMA 完成中断上下文中执行，只能调用 ISR 安全的函数，
 * 不能阻塞、延时或直接执行耗时操作。
 */
typedef bool (*lcd_transfer_done_callback_t)(void *user_context);

/**
 * @brief 初始化 BOX3 的 2.4 英寸 ST7789V2 LCD。
 *
 * 调用本函数前必须先完成 board_spi_init()。本函数只初始化 ST7789V2 面板，
 * 背光由 aw9523b_set_box3_lcd_backlight() 单独控制。本函数允许重复调用。
 * 显示坐标与 LVGL 一致：原点位于左上角，X 轴沿水平方向向右递增，范围为
 * 0~319；Y 轴沿竖直方向向下递增，范围为 0~239。
 */
esp_err_t lcd_init(void);

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
 * @brief 异步发送已经按 LCD 字节序排列的 DMA 像素缓冲区。
 *
 * @details 像素按照从左到右、从上到下的顺序连续排列，每个 RGB565 像素
 * 占两个字节，并且必须已经转换为 ST7789 所需的线路顺序：高字节在前、低
 * 字节在后。本函数不会复制数据，也不会交换 RGB565 的高低字节，而是让
 * SPI DMA 直接读取 pixels。
 *
 * pixels 必须满足以下条件：
 *
 * - 位于 SPI DMA 可访问的片内 RAM 或 PSRAM；
 * - 至少包含 width * height * sizeof(uint16_t) 字节；
 * - 从本函数返回 ESP_OK 开始，到 done_callback 执行前不能被修改、释放或
 *   交给其他模块复用。
 *
 * 函数会等待前一次 LCD 操作结束并取得 LCD 访问令牌，然后把本次传输提交
 * 给 ESP-IDF。若数据大于 SPI2 单次事务上限，ESP-IDF 会在内部自动拆分，
 * 只有最后一段 DMA 完成后才调用一次 done_callback。DMA 完成时驱动先释放
 * LCD 访问令牌，再在中断上下文中执行 done_callback，并把 user_context
 * 原样传给它。
 *
 * 返回 ESP_OK 仅表示传输已经成功提交，不表示像素已经全部发送。提交失败
 * 时函数会立即释放 LCD 访问令牌，且不会调用 done_callback。
 *
 * @param[in] x 目标矩形左上角的 X 坐标，范围为 0~319。
 * @param[in] y 目标矩形左上角的 Y 坐标，范围为 0~239。
 * @param[in] width 目标矩形宽度，沿 X 轴向右延伸，不能为 0。
 * @param[in] height 目标矩形高度，沿 Y 轴向下延伸，不能为 0。
 * @param[in] pixels 已按 LCD 字节序排列的 DMA 像素缓冲区，不能为 NULL。
 * @param[in] done_callback DMA 完成回调，不能为 NULL。
 * @param[in] user_context 传给 done_callback 的用户上下文，可以为 NULL。
 * @return ESP_OK 表示异步传输提交成功；LCD 未初始化时返回
 * ESP_ERR_INVALID_STATE；参数或矩形范围无效时返回 ESP_ERR_INVALID_ARG；
 * SPI/LCD 提交失败时返回对应的底层错误码。
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
 * (x, y) 是目标矩形左上角，width 沿 X 轴向右，height 沿 Y 轴向下。
 * 函数把源数据一次复制到全屏 PSRAM DMA 缓冲区，等待 SPI 传输完成后返回；
 * 返回后调用者可以立即复用 pixels。数据已经是“高字节、低字节”顺序，
 * 函数不会再次执行字节交换。
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
