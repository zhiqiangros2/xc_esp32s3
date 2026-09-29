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

/*
 * DMA 缓冲区能够保存完整的 240 像素 X 轴，以及连续 20 个 Y 坐标位置：
 *
 *     240 * 20 = 4800 个 RGB565 像素
 *     4800 * sizeof(uint16_t) = 9600 字节
 *
 * 全屏或大矩形传输时，驱动会沿 Y 轴分批发送，每批最多使用这块缓冲区
 * 容纳的数据量，因此不需要为整个 240x320 屏幕分配完整帧缓冲区。
 */
#define LCD_TRANSFER_BUFFER_PIXELS (LCD_X_RESOLUTION * LCD_TRANSFER_LINES)

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

static bool lcd_region_is_valid(uint16_t x,
                                uint16_t y,
                                uint16_t height,
                                uint16_t width)
{
    /* 高或宽为 0 时没有可绘制的像素，不是有效区域。 */
    if (height == 0 || width == 0) {
        return false;
    }

    /* 起点必须位于 X=0~239、Y=0~319 的 LCD 地址范围内。 */
    if (x >= LCD_X_RESOLUTION || y >= LCD_Y_RESOLUTION) {
        return false;
    }

    /*
     * height 沿 X 轴延伸，width 沿 Y 轴延伸。两个尺寸都不能超过对应
     * 坐标轴的剩余空间。例如 x=220 时，X 轴只剩 20 个像素，因此
     * height 最大只能是 20。
     */
    const uint16_t available_height = LCD_X_RESOLUTION - x;
    const uint16_t available_width = LCD_Y_RESOLUTION - y;
    if (height > available_height || width > available_width) {
        return false;
    }

    return true;
}

static esp_err_t lcd_fill_rect_locked(uint16_t x,
                                      uint16_t y,
                                      uint16_t height,
                                      uint16_t width,
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
     * DMA 缓冲区容量以“像素数”表示。每个 Y 坐标位置包含 height 个沿
     * X 轴排列的像素，因此用容量除以 height，可以得到一次传输最多覆盖
     * 多少个 Y 坐标。例如缓冲区有 4800 个像素、height=240 时，一次最多
     * 传输 20 个 Y 坐标位置。
     */
    const uint16_t maximum_y_pixels_per_transfer =
        (uint16_t)(LCD_TRANSFER_BUFFER_PIXELS / height);

    uint16_t current_transfer_y = y;
    uint16_t y_pixels_left_to_transfer = width;

    /* Y 方向长度超过单次传输容量时，沿 Y 轴拆成多批发送。 */
    while (y_pixels_left_to_transfer > 0) {
        uint16_t y_pixels_this_transfer = y_pixels_left_to_transfer;
        if (y_pixels_this_transfer > maximum_y_pixels_per_transfer) {
            y_pixels_this_transfer = maximum_y_pixels_per_transfer;
        }

        /* 清除旧的完成通知，确保下面等待的是当前这批 DMA 传输。 */
        while (xSemaphoreTake(lcd_dma_done_semaphore, 0) == pdTRUE) {
        }

        esp_err_t result = esp_lcd_panel_draw_bitmap(
            lcd_panel_handle,
            x,
            current_transfer_y,
            x + height,
            current_transfer_y + y_pixels_this_transfer,
            lcd_transfer_buffer);
        if (result != ESP_OK) {
            return result;
        }

        /* DMA 完成后才能复用 lcd_transfer_buffer 发送下一批数据。 */
        xSemaphoreTake(lcd_dma_done_semaphore, portMAX_DELAY);

        /* 下一批紧接当前 Y 区间，直到矩形在 Y 方向的像素全部发送完成。 */
        current_transfer_y += y_pixels_this_transfer;
        y_pixels_left_to_transfer -= y_pixels_this_transfer;
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
    if (lcd_mutex != NULL) {
        vSemaphoreDelete(lcd_mutex);
        lcd_mutex = NULL;
    }
}

/**
 * @brief 初始化板载 ST7789V2 LCD。
 *
 * 调用前必须先初始化 I2C、AW9523B 和 SPI2。函数会依次关闭背光、
 * 创建 LCD 面板、配置 X=240/Y=320 原生坐标、清屏并重新打开背光；重复调用安全。
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
     * 保持 ST7789 原生行列地址，不设置 MADCTL.MV（行列交换）位：
     *
     *  - X 范围为 0~239，对应屏幕上的竖直方向；
     *  - Y 范围为 0~319，对应屏幕上的水平方向。
     *
     * BOX3 的 LCD 在结构上横向安装，因此使用控制器原生地址时，物理屏幕的
     * 横轴是 Y 轴，而不是 X 轴。这也与 CHSC5432 输出的原始 240x320
     * 触摸坐标范围一致。
     */
    if (result == ESP_OK) {
        result = esp_lcd_panel_swap_xy(lcd_panel_handle, false);
    }

    /*
     * X、Y 均保持控制器原生递增方向，不设置 MADCTL.MX 和 MADCTL.MY。
     * 结合上面的 swap_xy=false，物理原点位于屏幕左下角：X 从下向上递增，
     * 范围为 0~239；Y 从左向右递增，范围为 0~319。
     *
     * 这里只改变 LCD 显存地址与物理方向的对应关系，不修改 RGB565 像素数据，
     * 也不转换触摸坐标。
     */
    if (result == ESP_OK) {
        result = esp_lcd_panel_mirror(lcd_panel_handle, false, false);
    }

    /* 显示开启前先清成黑色，避免背光点亮时出现旧显存内容。 */
    if (result == ESP_OK) {
        xSemaphoreTake(lcd_mutex, portMAX_DELAY);
        result = lcd_fill_rect_locked(0,
                                      0,
                                      LCD_X_RESOLUTION,
                                      LCD_Y_RESOLUTION,
                                      LCD_COLOR_BLACK);
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
                        uint16_t height,
                        uint16_t width,
                        uint16_t color)
{
    if (!lcd_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lcd_region_is_valid(x, y, height, width)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(lcd_mutex, portMAX_DELAY);
    esp_err_t result = lcd_fill_rect_locked(x, y, height, width, color);
    xSemaphoreGive(lcd_mutex);
    return result;
}

esp_err_t lcd_draw_pixels(uint16_t x,
                          uint16_t y,
                          uint16_t height,
                          uint16_t width,
                          const uint16_t *pixels)
{
    if (!lcd_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pixels == NULL || !lcd_region_is_valid(x, y, height, width)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 每个 Y 坐标包含 height 个连续的 X 方向像素。 */
    const uint16_t maximum_y_pixels_per_transfer =
        (uint16_t)(LCD_TRANSFER_BUFFER_PIXELS / height);
    uint16_t current_transfer_y = y;
    uint16_t y_pixels_left_to_transfer = width;
    size_t source_pixel_index = 0;
    esp_err_t result = ESP_OK;

    xSemaphoreTake(lcd_mutex, portMAX_DELAY);

    while (y_pixels_left_to_transfer > 0) {
        uint16_t y_pixels_this_transfer = y_pixels_left_to_transfer;
        if (y_pixels_this_transfer > maximum_y_pixels_per_transfer) {
            y_pixels_this_transfer = maximum_y_pixels_per_transfer;
        }

        const size_t pixels_this_transfer =
            (size_t)height * y_pixels_this_transfer;
        for (size_t pixel_index = 0;
             pixel_index < pixels_this_transfer;
             ++pixel_index) {
            lcd_transfer_buffer[pixel_index] =
                rgb565_to_wire_order(pixels[source_pixel_index + pixel_index]);
        }

        /* 清除旧通知，确保下面等待的是当前这次 DMA 传输。 */
        while (xSemaphoreTake(lcd_dma_done_semaphore, 0) == pdTRUE) {
        }

        result = esp_lcd_panel_draw_bitmap(
            lcd_panel_handle,
            x,
            current_transfer_y,
            x + height,
            current_transfer_y + y_pixels_this_transfer,
            lcd_transfer_buffer);
        if (result != ESP_OK) {
            break;
        }

        /* DMA 完成后才能改写共享传输缓冲区。 */
        xSemaphoreTake(lcd_dma_done_semaphore, portMAX_DELAY);

        current_transfer_y += y_pixels_this_transfer;
        y_pixels_left_to_transfer -= y_pixels_this_transfer;
        source_pixel_index += pixels_this_transfer;
    }

    xSemaphoreGive(lcd_mutex);
    return result;
}
