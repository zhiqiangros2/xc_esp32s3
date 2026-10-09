#include "lcd.h"

#include <stddef.h>
#include <string.h>

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

/*
 * DMA 缓冲区能够保存完整的 320x240 RGB565 画面：
 *
 *     320 * 240 = 76800 个 RGB565 像素
 *     76800 * sizeof(uint16_t) = 153600 字节
 */
#define LCD_TRANSFER_BUFFER_PIXELS (LCD_X_RESOLUTION * LCD_Y_RESOLUTION)

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

static esp_err_t lcd_fill_rect_internal(uint16_t x,
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
    const size_t pixel_count = (size_t)width * height;

    /* 独占 LCD 面板和共享 DMA 缓冲区，直到本次传输完成。 */
    xSemaphoreTake(lcd_access_semaphore, portMAX_DELAY);

    /* 全屏缓冲区可以容纳任意合法矩形，只填充本次传输实际使用的像素。 */
    for (size_t pixel_index = 0; pixel_index < pixel_count; ++pixel_index) {
        lcd_transfer_buffer[pixel_index] = color_in_wire_order;
    }

    /*
     * 清除可能残留的完成通知，确保下面等待的是本次 DMA 传输完成。
     */
    while (xSemaphoreTake(lcd_dma_done_semaphore, 0) == pdTRUE) {
    }

    const esp_err_t result = esp_lcd_panel_draw_bitmap(lcd_panel_handle,
                                                        x,
                                                        y,
                                                        x + width,
                                                        y + height,
                                                        lcd_transfer_buffer);
    if (result != ESP_OK) {
        xSemaphoreGive(lcd_access_semaphore);
        return result;
    }

    /* DMA 完成后才能复用 lcd_transfer_buffer。 */
    xSemaphoreTake(lcd_dma_done_semaphore, portMAX_DELAY);
    xSemaphoreGive(lcd_access_semaphore);
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
 * 调用前必须先初始化 SPI2。函数会创建 LCD 面板、配置 X=320/Y=240
 * 标准横屏坐标并清成黑色；背光由应用通过 AW9523B 单独控制。重复调用安全。
 */
esp_err_t lcd_init(void)
{
    /* LCD 已经初始化时直接返回，避免重复创建 SPI 和面板资源。 */
    if (lcd_initialized) {
        return ESP_OK;
    }

    esp_err_t result = ESP_OK;

    /* 访问令牌串行化同步绘图和异步 DMA；完成信号量供同步接口等待。 */
    lcd_access_semaphore = xSemaphoreCreateBinary();
    lcd_dma_done_semaphore = xSemaphoreCreateBinary();
    if (lcd_access_semaphore == NULL || lcd_dma_done_semaphore == NULL) {
        lcd_release_resources();
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(lcd_access_semaphore);

    /* 全屏 DMA 缓冲区放入 PSRAM，避免占用 153600 字节片内 RAM。 */
    lcd_transfer_buffer = board_spi_psram_dma_alloc(
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
        result = lcd_fill_rect_internal(0,
                                        0,
                                        LCD_X_RESOLUTION,
                                        LCD_Y_RESOLUTION,
                                        LCD_COLOR_BLACK);
    }
    /* 向 ST7789 发送 DISPON 命令以开启画面输出；此操作不会点亮 LCD 背光。 */
    if (result == ESP_OK) {
        result = esp_lcd_panel_disp_on_off(lcd_panel_handle, true);
    }
    /* 任一步骤失败都释放此前已经创建的 LCD 资源。 */
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ST7789V2: %s", esp_err_to_name(result));
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

    return lcd_fill_rect_internal(x, y, width, height, color);
}

/**
 * @brief 直接通过 SPI DMA 异步发送已转换字节序的 RGB565 像素。
 *
 * @details 本函数不使用 lcd_transfer_buffer，也不等待 DMA 完成。调用任务
 * 取得 lcd_access_semaphore 后提交 pixels；最后一段 DMA 完成时，
 * lcd_color_transfer_done() 在中断中释放访问令牌并执行用户回调。
 */
esp_err_t lcd_draw_rgb565_bytes_async(
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height,
    const uint8_t *pixels,
    lcd_transfer_done_callback_t done_callback,
    void *user_context)
{
    /* 面板、SPI IO 和访问令牌尚未就绪时不能提交传输。 */
    if (!lcd_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 回调用于通知调用方何时可以安全复用 pixels，因此不能为空。 */
    if (pixels == NULL || done_callback == NULL ||
        !lcd_region_is_valid(x, y, width, height)) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 访问令牌由本任务取得、DMA 完成中断释放。传输期间 LCD 不会被其他同步
     * 绘图接口使用，LVGL 传入的 DMA 缓冲区也会一直保持有效。
     */
    xSemaphoreTake(lcd_access_semaphore, portMAX_DELAY);
    /* 保存本次传输上下文，供 DMA 完成中断读取并清除。 */
    lcd_async_done_callback = done_callback;
    lcd_async_user_context = user_context;
    lcd_async_transfer_pending = true;

    /* ESP-IDF 会根据 SPI2 单次事务上限自动拆分较大的颜色数据。 */
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
 * GC0308 帧缓冲区已经按照 ST7789 的线路发送顺序保存。全屏 PSRAM DMA
 * 缓冲区可以容纳完整的 320x240 RGB565 画面，因此使用 memcpy() 原样复制
 * 后一次提交，不调用 rgb565_to_wire_order()，避免再次交换正确的字节序。
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

    const size_t transfer_byte_count =
        (size_t)width * height * sizeof(uint16_t);

    /* 独占 LCD 面板和共享 DMA 缓冲区，直到本次传输完成。 */
    xSemaphoreTake(lcd_access_semaphore, portMAX_DELAY);

    /* 数据已经是 LCD 线路字节序，直接复制完整矩形，不做逐像素转换。 */
    memcpy(lcd_transfer_buffer, pixels, transfer_byte_count);

    /* 清除旧完成通知，确保后面的等待对应当前这次 DMA 传输。 */
    while (xSemaphoreTake(lcd_dma_done_semaphore, 0) == pdTRUE) {
    }

    const esp_err_t result = esp_lcd_panel_draw_bitmap(lcd_panel_handle,
                                                        x,
                                                        y,
                                                        x + width,
                                                        y + height,
                                                        lcd_transfer_buffer);
    if (result == ESP_OK) {
        /* DMA 完成后才能复用 lcd_transfer_buffer。 */
        xSemaphoreTake(lcd_dma_done_semaphore, portMAX_DELAY);
    }

    xSemaphoreGive(lcd_access_semaphore);
    return result;
}
