#include "lcd.h"

#include <stddef.h>

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
#define LCD_TRANSFER_BUFFER_PIXELS (LCD_WIDTH_320 * LCD_TRANSFER_LINES)

static const char *TAG = "LCD";
static esp_lcd_panel_io_handle_t lcd_io_handle = NULL;
static esp_lcd_panel_handle_t lcd_panel_handle = NULL;
static SemaphoreHandle_t lcd_mutex = NULL;
static SemaphoreHandle_t lcd_dma_done_semaphore = NULL;
static uint16_t *lcd_transfer_buffer = NULL;
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

    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_context, &task_woken);
    return task_woken == pdTRUE;
}

static esp_err_t lcd_transfer_locked(uint16_t x,
                                     uint16_t y,
                                     uint16_t width,
                                     uint16_t height)
{
    /* 清除可能残留的旧完成通知，确保后续等待对应本次 DMA 传输。 */
    while (xSemaphoreTake(lcd_dma_done_semaphore, 0) == pdTRUE) {
    }

    esp_err_t result = esp_lcd_panel_draw_bitmap(lcd_panel_handle,
                                                  x,
                                                  y,
                                                  x + width,
                                                  y + height,
                                                  lcd_transfer_buffer);
    if (result != ESP_OK) {
        return result;
    }

    /* 等待本次 DMA 传输完成，防止 SPI 使用期间改写传输缓冲区。 */
    xSemaphoreTake(lcd_dma_done_semaphore, portMAX_DELAY);
    return ESP_OK;
}

static bool lcd_region_is_valid(uint16_t x,
                                uint16_t y,
                                uint16_t width,
                                uint16_t height)
{
    return width != 0 && height != 0 &&
           x < LCD_WIDTH_320 && y < LCD_HEIGHT_240 &&
           width <= LCD_WIDTH_320 - x && height <= LCD_HEIGHT_240 - y;
}

static esp_err_t lcd_fill_rect_locked(uint16_t x,
                                      uint16_t y,
                                      uint16_t width,
                                      uint16_t height,
                                      uint16_t color)
{
    const size_t rows_per_transfer = LCD_TRANSFER_BUFFER_PIXELS / width;
    const uint16_t wire_color = rgb565_to_wire_order(color);

    for (size_t i = 0; i < LCD_TRANSFER_BUFFER_PIXELS; ++i) {
        lcd_transfer_buffer[i] = wire_color;
    }

    uint16_t current_y = y;
    uint16_t remaining_rows = height;
    while (remaining_rows != 0) {
        const uint16_t rows = remaining_rows < rows_per_transfer
                                  ? remaining_rows
                                  : (uint16_t)rows_per_transfer;
        esp_err_t result = lcd_transfer_locked(x, current_y, width, rows);
        if (result != ESP_OK) {
            return result;
        }
        current_y += rows;
        remaining_rows -= rows;
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
    board_spi_deinit();
    if (lcd_dma_done_semaphore != NULL) {
        vSemaphoreDelete(lcd_dma_done_semaphore);
        lcd_dma_done_semaphore = NULL;
    }
    if (lcd_mutex != NULL) {
        vSemaphoreDelete(lcd_mutex);
        lcd_mutex = NULL;
    }
}

/**
 * @brief 初始化板载 ST7789V2 LCD。
 *
 * 调用前必须先初始化 I2C 和 AW9523B。函数会依次关闭背光、初始化 SPI2、
 * 创建 LCD 面板、配置 320x240 横屏模式、清屏并重新打开背光；重复调用安全。
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

    /* 互斥锁保护绘图缓冲区；二值信号量等待 SPI DMA 传输完成。 */
    lcd_mutex = xSemaphoreCreateMutex();
    lcd_dma_done_semaphore = xSemaphoreCreateBinary();
    if (lcd_mutex == NULL || lcd_dma_done_semaphore == NULL) {
        lcd_release_resources();
        return ESP_ERR_NO_MEM;
    }

    /* 按 20 行 RGB565 像素所需空间初始化 SPI2 的最大单次传输长度。 */
    result = board_spi_init(LCD_TRANSFER_BUFFER_PIXELS * sizeof(uint16_t));
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI2: %s", esp_err_to_name(result));
        lcd_release_resources();
        return result;
    }

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
        /* 只允许一个异步颜色传输排队，与单个 DMA 缓冲区配合使用。 */
        .trans_queue_depth = 1,
        /* 颜色数据 DMA 发送完成后，由回调通知缓冲区可以再次使用。 */
        .on_color_trans_done = lcd_color_transfer_done,
        /* 将传输完成信号量作为回调上下文传入。 */
        .user_ctx = lcd_dma_done_semaphore,
        /* ST7789V2 的命令和参数均按 8 位发送。 */
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
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
     * 交换 X/Y 地址轴，将控制器默认的 240x320 竖屏坐标转换为 320x240 横屏坐标。
     * ESP-IDF 会修改 ST7789 MADCTL 寄存器中的 MV 位。
     */
    if (result == ESP_OK) {
        result = esp_lcd_panel_swap_xy(lcd_panel_handle, true);
    }

    /*
     * X 轴保持不镜像，Y 轴镜像，使横屏坐标方向与 BOX3 屏幕的实际安装方向一致。
     * ESP-IDF 会据此设置 ST7789 MADCTL 寄存器中的 MX/MY 位。
     */
    if (result == ESP_OK) {
        result = esp_lcd_panel_mirror(lcd_panel_handle, false, true);
    }
    /* 显示开启前先清成黑色，避免背光点亮时出现旧显存内容。 */
    if (result == ESP_OK) {
        xSemaphoreTake(lcd_mutex, portMAX_DELAY);
        result = lcd_fill_rect_locked(0, 0, LCD_WIDTH_320, LCD_HEIGHT_240, LCD_COLOR_BLACK);
        xSemaphoreGive(lcd_mutex);
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
             "ST7789V2 ready: %ux%u, SPI2 %u MHz, CS=%d, DC=%d",
             LCD_WIDTH_320,
             LCD_HEIGHT_240,
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
    return lcd_fill_rect(0, 0, LCD_WIDTH_320, LCD_HEIGHT_240, color);
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

    xSemaphoreTake(lcd_mutex, portMAX_DELAY);
    esp_err_t result = lcd_fill_rect_locked(x, y, width, height, color);
    xSemaphoreGive(lcd_mutex);
    return result;
}

esp_err_t lcd_draw_bitmap(uint16_t x,
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

    const size_t rows_per_transfer = LCD_TRANSFER_BUFFER_PIXELS / width;
    uint16_t current_y = y;
    uint16_t remaining_rows = height;
    const uint16_t *source = pixels;
    esp_err_t result = ESP_OK;

    xSemaphoreTake(lcd_mutex, portMAX_DELAY);
    while (remaining_rows != 0) {
        const uint16_t rows = remaining_rows < rows_per_transfer
                                  ? remaining_rows
                                  : (uint16_t)rows_per_transfer;
        const size_t pixel_count = (size_t)width * rows;
        for (size_t i = 0; i < pixel_count; ++i) {
            lcd_transfer_buffer[i] = rgb565_to_wire_order(source[i]);
        }

        result = lcd_transfer_locked(x, current_y, width, rows);
        if (result != ESP_OK) {
            break;
        }

        source += pixel_count;
        current_y += rows;
        remaining_rows -= rows;
    }
    xSemaphoreGive(lcd_mutex);

    return result;
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
    const uint16_t bar_width = LCD_WIDTH_320 / (sizeof(colors) / sizeof(colors[0]));

    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
        esp_err_t result = lcd_fill_rect((uint16_t)(i * bar_width),
                                         0,
                                         bar_width,
                                         LCD_HEIGHT_240,
                                         colors[i]);
        if (result != ESP_OK) {
            return result;
        }
    }

    return ESP_OK;
}
