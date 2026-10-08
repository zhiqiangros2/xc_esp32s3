#include "lcd.h"

#include <stddef.h>
#include <string.h>

#include "aw9523b.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "spi.h"

#define LCD_CS_GPIO GPIO_NUM_47
#define LCD_DC_GPIO GPIO_NUM_48

#define LCD_PIXEL_CLOCK_HZ (60U * 1000U * 1000U)
#define LCD_TRANSFER_LINES 20U

/*
 * DMA 缓冲区能够保存完整的 320 像素宽度，以及连续 20 行像素：
 *
 *     320 * 20 = 6400 个 RGB565 像素
 *     6400 * sizeof(uint16_t) = 12800 字节
 *
 * 全屏或大矩形传输时，驱动会按行分批发送，因此不需要为整个 320x240
 * 屏幕分配完整帧缓冲区。
 */
#define LCD_TRANSFER_BUFFER_PIXELS (LCD_X_RESOLUTION * LCD_TRANSFER_LINES)

static const char *TAG = "LCD";
static esp_lcd_panel_io_handle_t lcd_io_handle = NULL;
static esp_lcd_panel_handle_t lcd_panel_handle = NULL;
static SemaphoreHandle_t lcd_access_semaphore = NULL;
/*
 * LCD DMA 传输完成二值信号量：
 *
 * - 空状态：当前没有可处理的 DMA 完成通知；
 * - 可取状态：DMA 完成回调已经发出一次完成通知。
 *
 * 它只表示“完成”或“未完成”，不累计完成次数。发送任务启动 DMA 后等待
 * 该信号量，回调在 DMA 结束时释放信号量，防止任务过早改写传输缓冲区。
 */
static SemaphoreHandle_t lcd_dma_done_semaphore = NULL;
static uint16_t *lcd_transfer_buffer = NULL;
static volatile bool lcd_async_transfer_pending = false;
static lcd_transfer_done_callback_t lcd_async_done_callback = NULL;
static void *lcd_async_user_context = NULL;
static bool lcd_initialized = false;

static uint16_t rgb565_to_wire_order(uint16_t color)
{
    return (uint16_t)((color << 8) | (color >> 8));
}

static esp_err_t lcd_backlight_write(bool on)
{
    return aw9523b_write_gpio(AW9523B_PORT_1,
                              AW9523B_BOX3_LCD_BACKLIGHT,
                              !on);
}

static bool lcd_color_transfer_done(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *event_data,
                                    void *user_context)
{
    (void)panel_io;
    (void)event_data;

    (void)user_context;

    BaseType_t task_woken = pdFALSE;
    if (lcd_async_transfer_pending) {
        lcd_transfer_done_callback_t callback = lcd_async_done_callback;
        void *callback_context = lcd_async_user_context;

        lcd_async_transfer_pending = false;
        lcd_async_done_callback = NULL;
        lcd_async_user_context = NULL;

        /* DMA 已不再读取外部缓冲区，可以开始下一次 LCD 传输。 */
        xSemaphoreGiveFromISR(lcd_access_semaphore, &task_woken);
        if (callback != NULL && callback(callback_context)) {
            task_woken = pdTRUE;
        }
    } else {
        /* 同步绘图接口仍由等待任务处理完成通知。 */
        xSemaphoreGiveFromISR(lcd_dma_done_semaphore, &task_woken);
    }

    return task_woken == pdTRUE;
}

static bool lcd_region_is_valid(uint16_t x,
                                uint16_t y,
                                uint16_t width,
                                uint16_t height)
{
    /* 高或宽为 0 时没有可绘制的像素，不是有效区域。 */
    if (width == 0 || height == 0) {
        return false;
    }

    /* 左上角必须位于 X=0~319、Y=0~239 的 LCD 地址范围内。 */
    if (x >= LCD_X_RESOLUTION || y >= LCD_Y_RESOLUTION) {
        return false;
    }

    /*
     * width 沿 X 轴向右延伸，height 沿 Y 轴向下延伸。两个尺寸都不能
     * 超过对应坐标轴的剩余空间。例如 x=300 时，屏幕右侧只剩 20 个
     * 像素，因此 width 最大只能是 20。
     */
    const uint16_t available_width = LCD_X_RESOLUTION - x;
    const uint16_t available_height = LCD_Y_RESOLUTION - y;
    if (width > available_width || height > available_height) {
        return false;
    }

    return true;
}

static esp_err_t lcd_fill_rect_locked(uint16_t x,
                                      uint16_t y,
                                      uint16_t width,
                                      uint16_t height,
                                      uint16_t color)
{
    /*
     * ESP32 内存中的 RGB565 是小端字节序，而 ST7789 在线路上要求先发送
     * 高字节。先交换颜色的两个字节，DMA 发送时就不需要再次转换。
     */
    const uint16_t color_in_wire_order = rgb565_to_wire_order(color);

    /*
     * 当前操作是纯色填充，缓冲区中的每个像素都相同。因此只需在发送前把
     * 整个 DMA 缓冲区填充一次，后面的每批传输可以重复使用同一块数据。
     */
    for (size_t pixel_index = 0;
         pixel_index < LCD_TRANSFER_BUFFER_PIXELS;
         ++pixel_index) {
        lcd_transfer_buffer[pixel_index] = color_in_wire_order;
    }

    /*
     * 每行包含 width 个像素，因此用 DMA 缓冲区容量除以 width，可以
     * 得到单次传输最多容纳的行数。例如矩形宽度为 320 时，一次最多发送
     * 20 行。
     */
    const uint16_t maximum_rows_per_transfer =
        (uint16_t)(LCD_TRANSFER_BUFFER_PIXELS / width);

    uint16_t transfer_start_y = y;
    uint16_t remaining_rows = height;

    /* 矩形高度超过单次传输容量时，沿 Y 轴拆成多批发送。 */
    while (remaining_rows > 0) {
        uint16_t current_row_count = remaining_rows;
        if (current_row_count > maximum_rows_per_transfer) {
            current_row_count = maximum_rows_per_transfer;
        }

        /*
         * 以 0 Tick 超时尝试获取二值信号量，不会在这里阻塞：
         *
         * - 返回 pdTRUE：存在上一次 DMA 遗留的完成通知，将其清除；
         * - 返回 pdFALSE：信号量已经为空，立即结束循环。
         *
         * 这样，启动下面的新 DMA 后，portMAX_DELAY 等到的一定是本次传输
         * 完成时由回调发出的通知。
         */
        while (xSemaphoreTake(lcd_dma_done_semaphore, 0) == pdTRUE) {
        }

        esp_err_t result = esp_lcd_panel_draw_bitmap(
            lcd_panel_handle,
            x,
            transfer_start_y,
            x + width,
            transfer_start_y + current_row_count,
            lcd_transfer_buffer);
        if (result != ESP_OK) {
            return result;
        }

        /* DMA 完成后才能复用 lcd_transfer_buffer 发送下一批数据。 */
        xSemaphoreTake(lcd_dma_done_semaphore, portMAX_DELAY);

        /* 下一批紧接当前行，直到矩形全部发送完成。 */
        transfer_start_y += current_row_count;
        remaining_rows -= current_row_count;
    }

    return ESP_OK;
}

static void lcd_release_resources(void)
{
    if (lcd_panel_handle != NULL) {
        esp_lcd_panel_del(lcd_panel_handle);
        lcd_panel_handle = NULL;
    }
    if (lcd_io_handle != NULL) {
        esp_lcd_panel_io_del(lcd_io_handle);
        lcd_io_handle = NULL;
    }
    if (lcd_transfer_buffer != NULL) {
        board_spi_dma_free(lcd_transfer_buffer);
        lcd_transfer_buffer = NULL;
    }
    if (lcd_dma_done_semaphore != NULL) {
        vSemaphoreDelete(lcd_dma_done_semaphore);
        lcd_dma_done_semaphore = NULL;
    }
    if (lcd_access_semaphore != NULL) {
        vSemaphoreDelete(lcd_access_semaphore);
        lcd_access_semaphore = NULL;
    }

    lcd_async_transfer_pending = false;
    lcd_async_done_callback = NULL;
    lcd_async_user_context = NULL;
}

/**
 * @brief 初始化板载 ST7789V2 LCD。
 *
 * 调用前必须先初始化 I2C、AW9523B 和 SPI2。函数会依次关闭背光、
 * 创建 LCD 面板、配置 X=320/Y=240 标准横屏坐标、清屏并重新打开背光；
 * 重复调用安全。
 */
esp_err_t lcd_init(void)
{
    /* LCD 已经初始化时直接返回，避免重复创建 SPI 和面板资源。 */
    if (lcd_initialized) {
        return ESP_OK;
    }

    /* 初始化期间关闭背光，避免屏幕配置完成前显示随机内容。 */
    esp_err_t result = lcd_backlight_write(false);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to turn off backlight: %s", esp_err_to_name(result));
        return result;
    }

    /* 访问令牌串行化同步绘图和异步 DMA；完成信号量供同步接口等待。 */
    lcd_access_semaphore = xSemaphoreCreateBinary();
    lcd_dma_done_semaphore = xSemaphoreCreateBinary();
    if (lcd_access_semaphore == NULL || lcd_dma_done_semaphore == NULL) {
        lcd_release_resources();
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(lcd_access_semaphore);

    /* 分配片内 DMA 缓冲区，填充和位图绘制均通过该缓冲区分块发送。 */
    lcd_transfer_buffer = board_spi_dma_alloc(
        LCD_TRANSFER_BUFFER_PIXELS * sizeof(uint16_t));
    if (lcd_transfer_buffer == NULL) {
        lcd_release_resources();
        return ESP_ERR_NO_MEM;
    }

    /* 配置 ST7789V2 的 SPI 命令/数据接口以及 DMA 完成回调。 */
    const esp_lcd_panel_io_spi_config_t io_config = {
        /* LCD 片选引脚，传输期间由驱动自动拉低。 */
        .cs_gpio_num = LCD_CS_GPIO,
        /* 命令/数据选择引脚：低电平发送命令，高电平发送参数或像素数据。 */
        .dc_gpio_num = LCD_DC_GPIO,
        /* SPI 模式 0：时钟空闲时为低电平，在第一个时钟边沿采样数据。 */
        .spi_mode = 0,
        /* LCD 的 SPI 工作时钟为 60 MHz。 */
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        /* 允许全屏数据拆分后连续排队，和参考工程保持相同队列深度。 */
        .trans_queue_depth = 7,
        /* 颜色数据 DMA 发送完成后，由回调通知缓冲区可以再次使用。 */
        .on_color_trans_done = lcd_color_transfer_done,
        .user_ctx = NULL,
        /* ST7789V2 的命令和参数均按 8 位发送。 */
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            /* LVGL 全屏缓冲位于 PSRAM，由 ESP32-S3 GDMA 直接读取。 */
            .psram_dma_direct = 1,
        },
    };

    result = esp_lcd_new_panel_io_spi(
                                      (esp_lcd_spi_bus_handle_t)board_spi_get_host(),
                                      &io_config,
                                      &lcd_io_handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to attach LCD to SPI2: %s", esp_err_to_name(result));
        lcd_release_resources();
        return result;
    }

    /* 配置 LCD 复位方式和 RGB565 数据格式。 */
    const esp_lcd_panel_dev_config_t panel_config = {
        /* LCD RESX 与 ESP32-S3 CHIP_PU 共用 ESP_LCD_RESET 网络，并非独立 GPIO。 */
        .reset_gpio_num = GPIO_NUM_NC,
        /* 像素颜色通道按照 RGB 顺序解析。 */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        /* RGB565 像素数据先发送高字节，再发送低字节。 */
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        /* 每个像素使用 16 位 RGB565 格式。 */
        .bits_per_pixel = 16,
    };

    /*
     * 创建 ST7789 面板实例，将上面的 SPI IO 句柄和面板参数交给 ESP-IDF。
     * 此步骤只创建驱动对象，还不会完成 ST7789 寄存器初始化。
     */
    result = esp_lcd_new_panel_st7789(lcd_io_handle,
                                      &panel_config,
                                      &lcd_panel_handle);

    /*
     * 只有面板对象创建成功才执行复位。后续步骤也使用相同判断：任一步失败后，
     * result 不再等于 ESP_OK，剩余步骤会被跳过，最后进入统一的错误清理流程。
     * reset_gpio_num 为 GPIO_NUM_NC，因此驱动通过 SPI 发送 SWRESET 软件复位命令，
     * 不会尝试控制与 CHIP_PU 共用的 ESP_LCD_RESET 硬件网络。
     */
    if (result == ESP_OK) {
        result = esp_lcd_panel_reset(lcd_panel_handle);
    }

    /*
     * 初始化 ST7789 控制器：退出睡眠模式，并写入颜色格式、RGB 顺序和数据字节序等
     * 基础寄存器。此步骤完成后才能设置显示方向和写入像素数据。
     */
    if (result == ESP_OK) {
        result = esp_lcd_panel_init(lcd_panel_handle);
    }

    /*
     * 开启 ST7789 颜色反转模式。该屏幕面板需要发送 INVON 命令才能正确显示颜色，
     * 否则画面颜色会呈现反相效果。
     */
    if (result == ESP_OK) {
        result = esp_lcd_panel_invert_color(lcd_panel_handle, true);
    }

    /*
     * ST7789 原生地址是 X=240、Y=320。设置 MADCTL.MV 后交换行列地址，
     * 使绘图接口改用标准横屏范围：X=0~319、Y=0~239。
     */
    if (result == ESP_OK) {
        result = esp_lcd_panel_swap_xy(lcd_panel_handle, true);
    }

    /*
     * BOX3 横屏安装方向需要反转交换后的 X 地址方向。结合上面的行列交换，
     * 最终坐标与 LVGL 一致：原点位于左上角，X 从左向右递增，Y 从上向下
     * 递增。
     *
     * 这里只改变 LCD 显存地址映射，不修改传入的 RGB565 像素数据。触摸
     * 驱动会单独把 CHSC5432 原始坐标转换到同一坐标系。
     */
    if (result == ESP_OK) {
        result = esp_lcd_panel_mirror(lcd_panel_handle, true, false);
    }

    /* 显示开启前先清成黑色，避免背光点亮时出现旧显存内容。 */
    if (result == ESP_OK) {
        xSemaphoreTake(lcd_access_semaphore, portMAX_DELAY);
        result = lcd_fill_rect_locked(0,
                                      0,
                                      LCD_X_RESOLUTION,
                                      LCD_Y_RESOLUTION,
                                      LCD_COLOR_BLACK);
        xSemaphoreGive(lcd_access_semaphore);
    }
    /* 向 ST7789 发送 DISPON 命令以开启画面输出；此操作不会点亮 LCD 背光。 */
    if (result == ESP_OK) {
        result = esp_lcd_panel_disp_on_off(lcd_panel_handle, true);
    }
    /* 面板输出开启后，通过 AW9523B P1_0 输出低电平点亮 LCD 背光。 */
    if (result == ESP_OK) {
        result = lcd_backlight_write(true);
    }
    /* 任一步骤失败都关闭背光并释放此前已经创建的资源。 */
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ST7789V2: %s", esp_err_to_name(result));
        lcd_backlight_write(false);
        lcd_release_resources();
        return result;
    }

    /* 所有步骤完成后才发布初始化状态。 */
    lcd_initialized = true;
    ESP_LOGI(TAG,
             "ST7789V2 ready: X=%u, Y=%u, SPI2 %u MHz, CS=%d, DC=%d",
             LCD_X_RESOLUTION,
             LCD_Y_RESOLUTION,
             LCD_PIXEL_CLOCK_HZ / 1000000U,
             LCD_CS_GPIO,
             LCD_DC_GPIO);
    return ESP_OK;
}

esp_err_t lcd_backlight_set(bool on)
{
    if (!lcd_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    return lcd_backlight_write(on);
}

esp_err_t lcd_clear(uint16_t color)
{
    return lcd_fill_rect(0, 0, LCD_X_RESOLUTION, LCD_Y_RESOLUTION, color);
}

esp_err_t lcd_fill_rect(uint16_t x,
                        uint16_t y,
                        uint16_t width,
                        uint16_t height,
                        uint16_t color)
{
    if (!lcd_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lcd_region_is_valid(x, y, width, height)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(lcd_access_semaphore, portMAX_DELAY);
    esp_err_t result = lcd_fill_rect_locked(x, y, width, height, color);
    xSemaphoreGive(lcd_access_semaphore);
    return result;
}

/**
 * @brief 把连续的 RGB565 像素数组绘制到 LCD 的指定矩形区域。
 *
 * @details pixels 采用常见的逐行排列：先保存最上面一行，并在一行内从
 * 左向右保存，然后继续保存下一行。因此源数组下标和目标坐标的关系是：
 *
 *     source_index = row * width + column
 *     LCD_X        = x + column
 *     LCD_Y        = y + row
 *
 * 例如 width=4、height=3 时，数组与屏幕位置的关系如下：
 *
 *                      LCD X 向右
 *       (x, y) +----------+----------+----------+----------+
 *              | pixels[0]| pixels[1]| pixels[2]| pixels[3]|
 *              +----------+----------+----------+----------+
 *              | pixels[4]| pixels[5]| pixels[6]| pixels[7]|
 *              +----------+----------+----------+----------+
 *              | pixels[8]| pixels[9]|pixels[10]|pixels[11]|
 *              +----------+----------+----------+----------+
 *              |
 *              v LCD Y 向下
 *
 * 内部 DMA 缓冲区只能容纳 LCD_TRANSFER_BUFFER_PIXELS 个像素，所以较大
 * 矩形会按完整行分成多批。每批执行以下步骤：
 *
 *  1. 计算本批最多可以传输多少行；
 *  2. 把对应源像素复制到 DMA 缓冲区，同时把 RGB565 的两个字节转换为
 *     ST7789 在线路上需要的高字节先发顺序；
 *  3. 启动当前矩形分块的 SPI DMA 传输；
 *  4. 等待 DMA 完成，再复用同一个传输缓冲区处理下一批。
 *
 * lcd_access_semaphore 会保证多个任务不会同时使用 LCD 和共享 DMA 缓冲区。
 * 函数等待最后一批 DMA 完成后才释放访问令牌并返回，因此返回后 pixels
 * 可以立即复用。
 *
 * @param[in] x 目标矩形左上角的 LCD X 坐标。
 * @param[in] y 目标矩形左上角的 LCD Y 坐标。
 * @param[in] width 目标矩形宽度，沿 X 轴向右延伸。
 * @param[in] height 目标矩形高度，沿 Y 轴向下延伸。
 * @param[in] pixels 至少包含 width * height 个元素的 RGB565 源像素数组。
 * @return ESP_OK 全部像素绘制成功；LCD 未初始化时返回
 * ESP_ERR_INVALID_STATE；参数无效时返回 ESP_ERR_INVALID_ARG；底层 LCD
 * 传输失败时返回 esp_lcd_panel_draw_bitmap() 的错误码。
 */
esp_err_t lcd_draw_pixels(uint16_t x,
                          uint16_t y,
                          uint16_t width,
                          uint16_t height,
                          const uint16_t *pixels)
{
    if (!lcd_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pixels == NULL || !lcd_region_is_valid(x, y, width, height)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* DMA 缓冲区容量除以每行像素数，得到一次最多能够发送的完整行数。 */
    const uint16_t maximum_rows_per_transfer =
        (uint16_t)(LCD_TRANSFER_BUFFER_PIXELS / width);

    /* 第一批从 y 开始；后续每完成一批，就继续发送其下方尚未发送的行。 */
    uint16_t transfer_start_y = y;
    uint16_t remaining_rows = height;

    /* 指向 pixels 中下一批源数据的第一个像素。 */
    size_t source_pixel_index = 0;
    esp_err_t result = ESP_OK;

    /* 独占 LCD 面板和共享 DMA 缓冲区，避免其他任务同时绘图。 */
    xSemaphoreTake(lcd_access_semaphore, portMAX_DELAY);

    while (remaining_rows > 0) {
        /* 最后一批可能不足最大行数，只发送实际剩余的完整行。 */
        uint16_t current_row_count = remaining_rows;
        if (current_row_count > maximum_rows_per_transfer) {
            current_row_count = maximum_rows_per_transfer;
        }

        /*
         * 当前批次的像素总数 = 每行像素数 * 本批次行数。复制时顺便交换
         * RGB565 的高低字节，让 DMA 缓冲区中的数据可以直接发给 ST7789。
         */
        const size_t current_transfer_pixel_count =
            (size_t)width * current_row_count;
        for (size_t pixel_index = 0;
             pixel_index < current_transfer_pixel_count;
             ++pixel_index) {
            lcd_transfer_buffer[pixel_index] =
                rgb565_to_wire_order(pixels[source_pixel_index + pixel_index]);
        }

        /*
         * 以 0 Tick 超时清除二值信号量中可能残留的旧 DMA 完成通知；操作
         * 不会阻塞。清空后，下面的等待只会被当前这次 DMA 的完成回调唤醒。
         */
        while (xSemaphoreTake(lcd_dma_done_semaphore, 0) == pdTRUE) {
        }

        result = esp_lcd_panel_draw_bitmap(
            lcd_panel_handle,
            x,
            transfer_start_y,
            x + width,
            transfer_start_y + current_row_count,
            lcd_transfer_buffer);
        if (result != ESP_OK) {
            break;
        }

        /* DMA 完成后才能改写共享传输缓冲区。 */
        xSemaphoreTake(lcd_dma_done_semaphore, portMAX_DELAY);

        /* 移到下一批的屏幕起始行和源数组起始像素。 */
        transfer_start_y += current_row_count;
        remaining_rows -= current_row_count;
        source_pixel_index += current_transfer_pixel_count;
    }

    xSemaphoreGive(lcd_access_semaphore);
    return result;
}

esp_err_t lcd_draw_rgb565_bytes_async(
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height,
    const uint8_t *pixels,
    lcd_transfer_done_callback_t done_callback,
    void *user_context)
{
    if (!lcd_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (pixels == NULL || done_callback == NULL ||
        !lcd_region_is_valid(x, y, width, height)) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 访问令牌由本任务取得、DMA 完成中断释放。传输期间 LCD 不会被其他同步
     * 绘图接口使用，LVGL 传入的 DMA 缓冲区也会一直保持有效。
     */
    xSemaphoreTake(lcd_access_semaphore, portMAX_DELAY);
    lcd_async_done_callback = done_callback;
    lcd_async_user_context = user_context;
    lcd_async_transfer_pending = true;

    const esp_err_t result = esp_lcd_panel_draw_bitmap(lcd_panel_handle,
                                                        x,
                                                        y,
                                                        x + width,
                                                        y + height,
                                                        pixels);
    if (result != ESP_OK) {
        /* 未启动 DMA 时不会产生完成中断，必须在当前任务中归还访问令牌。 */
        lcd_async_transfer_pending = false;
        lcd_async_done_callback = NULL;
        lcd_async_user_context = NULL;
        xSemaphoreGive(lcd_access_semaphore);
    }

    return result;
}

/**
 * @brief 绘制高字节在前的 RGB565 原始字节流。
 *
 * GC0308 帧缓冲区已经按照 ST7789 的线路发送顺序保存。这里按完整行把数据
 * 拆成多批，使用 memcpy() 原样复制到片内 DMA 缓冲区，不调用
 * rgb565_to_wire_order()。这样既能从 PSRAM 帧缓冲区稳定发送，又不会把正确
 * 的摄像头字节序再次交换。
 */
esp_err_t lcd_draw_rgb565_bytes(uint16_t x,
                                uint16_t y,
                                uint16_t width,
                                uint16_t height,
                                const uint8_t *pixels)
{
    if (!lcd_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pixels == NULL || !lcd_region_is_valid(x, y, width, height)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* DMA 缓冲区除以每行像素数，得到一次能够发送的最大完整行数。 */
    const uint16_t maximum_rows_per_transfer =
        (uint16_t)(LCD_TRANSFER_BUFFER_PIXELS / width);
    uint16_t transfer_start_y = y;
    uint16_t remaining_rows = height;
    size_t source_byte_index = 0;
    esp_err_t result = ESP_OK;

    /* 独占 LCD 面板和传输缓冲区，防止其他任务在帧传输中间插入绘图。 */
    xSemaphoreTake(lcd_access_semaphore, portMAX_DELAY);

    while (remaining_rows > 0) {
        uint16_t current_row_count = remaining_rows;
        if (current_row_count > maximum_rows_per_transfer) {
            current_row_count = maximum_rows_per_transfer;
        }

        /*
         * 一行有 width 个像素，每个 RGB565 像素固定占 2 字节。摄像头数据
         * 已经是高字节在前，因此整批原样复制，不逐像素转换。
         */
        const size_t current_transfer_byte_count =
            (size_t)width * current_row_count * sizeof(uint16_t);
        memcpy(lcd_transfer_buffer,
               pixels + source_byte_index,
               current_transfer_byte_count);

        /* 清除旧完成通知，确保后面的等待对应当前这次 DMA 传输。 */
        while (xSemaphoreTake(lcd_dma_done_semaphore, 0) == pdTRUE) {
        }

        result = esp_lcd_panel_draw_bitmap(
            lcd_panel_handle,
            x,
            transfer_start_y,
            x + width,
            transfer_start_y + current_row_count,
            lcd_transfer_buffer);
        if (result != ESP_OK) {
            break;
        }

        /* 当前 DMA 完成后，才允许下一批覆盖共用传输缓冲区。 */
        xSemaphoreTake(lcd_dma_done_semaphore, portMAX_DELAY);

        transfer_start_y += current_row_count;
        remaining_rows -= current_row_count;
        source_byte_index += current_transfer_byte_count;
    }

    xSemaphoreGive(lcd_access_semaphore);
    return result;
}
