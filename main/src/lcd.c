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
#include "lcdfont.h"
#include "spi.h"

#define LCD_CS_GPIO GPIO_NUM_47
#define LCD_DC_GPIO GPIO_NUM_48

#define LCD_PIXEL_CLOCK_HZ (60U * 1000U * 1000U)
#define LCD_TRANSFER_LINES 20U
#define LCD_TRANSFER_BUFFER_PIXELS (LCD_X_RESOLUTION * LCD_TRANSFER_LINES)

static const char *TAG = "LCD";
static esp_lcd_panel_io_handle_t lcd_io_handle = NULL;
static esp_lcd_panel_handle_t lcd_panel_handle = NULL;
static SemaphoreHandle_t lcd_mutex = NULL;
static SemaphoreHandle_t lcd_dma_done_semaphore = NULL;
static uint16_t *lcd_transfer_buffer = NULL;
static bool lcd_initialized = false;

typedef struct {
    const unsigned char *bitmap;
    uint8_t width;
    uint8_t height;
    uint8_t bytes_per_row;
} lcd_ascii_glyph_t;

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
                                uint16_t width,
                                uint16_t height)
{
    /* 宽或高为 0 时没有可绘制的像素，不是有效区域。 */
    if (width == 0 || height == 0) {
        return false;
    }

    /* 起点必须位于 X=0~239、Y=0~319 的 LCD 地址范围内。 */
    if (x >= LCD_X_RESOLUTION || y >= LCD_Y_RESOLUTION) {
        return false;
    }

    /*
     * 从起点到两个坐标轴末端的空间必须足以容纳整个矩形。
     * 例如 x=220 时，X 轴只剩 20 个像素，因此 width 最大只能是 20。
     */
    const uint16_t available_width = LCD_X_RESOLUTION - x;
    const uint16_t available_height = LCD_Y_RESOLUTION - y;
    if (width > available_width || height > available_height) {
        return false;
    }

    return true;
}

static bool lcd_get_ascii_glyph(char character,
                                uint8_t font_height,
                                lcd_ascii_glyph_t *glyph)
{
    const unsigned char ascii_code = (unsigned char)character;
    if (glyph == NULL || ascii_code < 0x20U || ascii_code > 0x7EU) {
        return false;
    }

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

static esp_err_t lcd_show_char_locked(uint16_t x,
                                      uint16_t y,
                                      char character,
                                      uint8_t font_height,
                                      uint16_t foreground_color,
                                      uint16_t background_color)
{
    lcd_ascii_glyph_t glyph;
    if (!lcd_get_ascii_glyph(character, font_height, &glyph) ||
        !lcd_region_is_valid(x, y, glyph.height, glyph.width)) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint16_t foreground_in_wire_order =
        rgb565_to_wire_order(foreground_color);
    const uint16_t background_in_wire_order =
        rgb565_to_wire_order(background_color);

    /*
     * 字模原始数据按“从上到下逐行、每行从左到右”保存。当前 LCD 的 X 轴
     * 从下向上，Y 轴从左向右，因此显示一个正向字符时需要重新排列像素：
     *
     *  - 字模的行映射到 LCD 的 X 轴，并反转顺序，使字模顶部位于较大的 X；
     *  - 字模的列映射到 LCD 的 Y 轴，保持从左向右的顺序。
     *
     * DMA 矩形的 X 方向长度为字符高度，Y 方向长度为字符宽度。这样字符
     * 保持正向显示，后续字符串可以沿 Y 轴从左向右排列。
     */
    for (uint8_t y_offset = 0; y_offset < glyph.width; ++y_offset) {
        for (uint8_t x_offset = 0; x_offset < glyph.height; ++x_offset) {
            const uint8_t source_row = glyph.height - 1U - x_offset;
            const uint8_t source_column = y_offset;
            const size_t byte_index =
                (size_t)source_row * glyph.bytes_per_row + source_column / 8U;
            const uint8_t bit_mask =
                (uint8_t)(0x80U >> (source_column % 8U));
            const bool pixel_is_foreground =
                (glyph.bitmap[byte_index] & bit_mask) != 0;

            lcd_transfer_buffer[(size_t)y_offset * glyph.height + x_offset] =
                pixel_is_foreground ? foreground_in_wire_order
                                    : background_in_wire_order;
        }
    }

    /* 清除旧通知，随后提交本字符的整块像素并等待 DMA 完成。 */
    while (xSemaphoreTake(lcd_dma_done_semaphore, 0) == pdTRUE) {
    }

    esp_err_t result = esp_lcd_panel_draw_bitmap(lcd_panel_handle,
                                                  x,
                                                  y,
                                                  x + glyph.height,
                                                  y + glyph.width,
                                                  lcd_transfer_buffer);
    if (result != ESP_OK) {
        return result;
    }

    xSemaphoreTake(lcd_dma_done_semaphore, portMAX_DELAY);
    return ESP_OK;
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
     * DMA 缓冲区容量以“像素数”表示。用容量除以矩形宽度，可以得到一次
     * 传输最多容纳多少个完整行。例如缓冲区有 4800 个像素、X 方向长度为 240，
     * 则每次最多传输 20 行。
     */
    const uint16_t maximum_rows_per_transfer =
        (uint16_t)(LCD_TRANSFER_BUFFER_PIXELS / width);

    uint16_t current_transfer_y = y;
    uint16_t rows_left_to_transfer = height;

    /* Y 方向长度超过单次传输容量时，沿 Y 轴拆成多批发送。 */
    while (rows_left_to_transfer > 0) {
        uint16_t rows_this_transfer = rows_left_to_transfer;
        if (rows_this_transfer > maximum_rows_per_transfer) {
            rows_this_transfer = maximum_rows_per_transfer;
        }

        /* 清除旧的完成通知，确保下面等待的是当前这批 DMA 传输。 */
        while (xSemaphoreTake(lcd_dma_done_semaphore, 0) == pdTRUE) {
        }

        esp_err_t result = esp_lcd_panel_draw_bitmap(
            lcd_panel_handle,
            x,
            current_transfer_y,
            x + width,
            current_transfer_y + rows_this_transfer,
            lcd_transfer_buffer);
        if (result != ESP_OK) {
            return result;
        }

        /* DMA 完成后才能复用 lcd_transfer_buffer 发送下一批数据。 */
        xSemaphoreTake(lcd_dma_done_semaphore, portMAX_DELAY);

        /* 下一批紧接在本批下方，直到目标矩形的所有行都发送完成。 */
        current_transfer_y += rows_this_transfer;
        rows_left_to_transfer -= rows_this_transfer;
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

esp_err_t lcd_show_char(uint16_t x,
                        uint16_t y,
                        char character,
                        uint8_t font_height,
                        uint16_t foreground_color,
                        uint16_t background_color)
{
    if (!lcd_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    lcd_ascii_glyph_t glyph;
    if (!lcd_get_ascii_glyph(character, font_height, &glyph) ||
        !lcd_region_is_valid(x, y, glyph.height, glyph.width)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(lcd_mutex, portMAX_DELAY);
    esp_err_t result = lcd_show_char_locked(x,
                                             y,
                                             character,
                                             font_height,
                                             foreground_color,
                                             background_color);
    xSemaphoreGive(lcd_mutex);
    return result;
}

esp_err_t lcd_show_string(uint16_t x,
                          uint16_t y,
                          uint16_t x_size,
                          uint16_t y_size,
                          uint8_t font_height,
                          const char *text,
                          uint16_t foreground_color,
                          uint16_t background_color)
{
    if (!lcd_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (text == NULL || !lcd_region_is_valid(x, y, x_size, y_size)) {
        return ESP_ERR_INVALID_ARG;
    }

    lcd_ascii_glyph_t glyph;
    if (!lcd_get_ascii_glyph(' ', font_height, &glyph)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (x_size < glyph.height || y_size < glyph.width) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 先检查完整字符串，避免绘制一半后才发现不支持的字符。 */
    for (const char *current = text; *current != '\0'; ++current) {
        const unsigned char character = (unsigned char)*current;
        if (character != '\n' && character != '\r' &&
            (character < 0x20U || character > 0x7EU)) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    const uint32_t text_area_x_end = (uint32_t)x + x_size;
    const uint32_t text_area_y_end = (uint32_t)y + y_size;
    uint32_t character_x = x;
    uint32_t character_y = y;

    xSemaphoreTake(lcd_mutex, portMAX_DELAY);

    esp_err_t result = ESP_OK;
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

        result = lcd_show_char_locked((uint16_t)character_x,
                                      (uint16_t)character_y,
                                      *current,
                                      font_height,
                                      foreground_color,
                                      background_color);
        if (result != ESP_OK) {
            break;
        }

        character_y += glyph.width;
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
