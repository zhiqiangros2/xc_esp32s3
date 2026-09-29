#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "aw9523b.h"
#include "bsp_info.h"
#include "display.h"
#include "fatfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c.h"
#include "interrupt_manager.h"
#include "key_interrupt.h"
#include "lcd.h"
#include "littlefs.h"
#include "nvs_flash.h"
#include "sd_fatfs.h"
#include "spi.h"
#include "tp.h"

static const char *TAG = "BOX3";

void app_main(void)
{
    /* NVS 用于蓝牙控制器的 PHY 校准和配对信息，必须先于蓝牙功能初始化。 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    vTaskDelay(pdMS_TO_TICKS(5000));

    bsp_info_print();

    /* K0 独立连接 GPIO0，不经过 I2C 或 GPIO42 共享中断管理器。 */
    ESP_ERROR_CHECK(key_interrupt_init());

    /* 初始化板级 I2C0 总线，后续 AW9523B 和触摸屏共用该总线。 */
    ESP_ERROR_CHECK(board_i2c_init());

    /* 初始化 AW9523B，并确保板载红、蓝 LED 均处于熄灭状态。 */
    ESP_ERROR_CHECK(aw9523b_init());
    ESP_ERROR_CHECK(aw9523b_set_box3_led(AW9523B_BOX3_LED_RED, false));
    ESP_ERROR_CHECK(aw9523b_set_box3_led(AW9523B_BOX3_LED_BLUE, false));

    /* 配置 AW9523B 的 K1/K2 输入变化中断。 */
    ESP_ERROR_CHECK(aw9523b_interrupt_init());

    /* CHSC5432 通过 I2C 读取触点，复位信号由 AW9523B P1_7 控制。 */
    ESP_ERROR_CHECK(tp_init());

    /* GPIO42 触发后依次查询 AW9523B 和 CHSC5432，判断实际中断来源。 */
    ESP_ERROR_CHECK(interrupt_manager_init(INTERRUPT_SOURCE_AW9523B |
                                           INTERRUPT_SOURCE_TOUCH));

    /*
     * LCD 和 SD 共用 SPI2 的 SCLK=GPIO15、MOSI=GPIO16、MISO=GPIO17，
     * 这里只初始化一次总线；两个设备之后分别使用 LCD_CS=GPIO47 和
     * SD_CS=GPIO18。LCD 和 SD 都会注册成 SPI2 上的独立设备，ESP-IDF
     * 内部的 SPI bus lock 会把同时到来的读写请求自动排队，同一时刻只
     * 允许一个设备执行事务，因此不需要手动切换 CS，也不要再增加一把
     * 跨 LCD/SD 的全局 SPI mutex。
     */
    ESP_ERROR_CHECK(board_spi_init(BOARD_SPI_MAX_TRANSFER_SIZE));

    /*
     * 基础外设就绪后初始化 LCD，并显示八色竖向测试色条，用于检查屏幕
     * 刷新、RGB565 颜色以及横屏显示方向是否正常。
     */
    ESP_ERROR_CHECK(lcd_init());
    ESP_ERROR_CHECK(lcd_show_test_pattern());

    /* SD 与 LCD 共用 SPI2；SD 挂载失败时只记录警告，不影响其他功能。 */
    esp_err_t sd_result = sd_init();
    if (sd_result == ESP_OK) {
        /* 挂载成功后执行 SD FATFS 的写入、读回校验和重命名测试。 */
        const esp_err_t sd_test_result = sd_fatfs_test();
        if (sd_test_result != ESP_OK) {
            ESP_LOGW(TAG,
                     "SD FATFS test failed: %s",
                     esp_err_to_name(sd_test_result));
        }
    } else {
        ESP_LOGW(TAG,
                 "SD unavailable; continuing without storage: %s",
                 esp_err_to_name(sd_result));
    }

    /* 挂载内部 Flash 的 vfs FATFS 分区；失败时只记录警告，不触发重启。 */
    esp_err_t fatfs_result = fatfs_init();
    if (fatfs_result == ESP_OK) {
        /* 挂载成功后执行内部 Flash FATFS 的文件读写测试。 */
        const esp_err_t fatfs_test_result = fatfs_test();
        if (fatfs_test_result != ESP_OK) {
            ESP_LOGW(TAG,
                     "Flash FATFS test failed: %s",
                     esp_err_to_name(fatfs_test_result));
        }
    } else {
        ESP_LOGW(TAG,
                 "Flash FATFS unavailable; continuing without storage: %s",
                 esp_err_to_name(fatfs_result));
    }

    /* 挂载内部 Flash 的 LittleFS 分区；失败时只记录警告，不触发重启。 */
    esp_err_t littlefs_result = littlefs_init();
    if (littlefs_result == ESP_OK) {
        /* 挂载成功后执行 LittleFS 的文件读写测试。 */
        const esp_err_t littlefs_test_result = littlefs_test();
        if (littlefs_test_result != ESP_OK) {
            ESP_LOGW(TAG,
                     "LittleFS test failed: %s",
                     esp_err_to_name(littlefs_test_result));
        }
    } else {
        ESP_LOGW(TAG,
                 "LittleFS unavailable; continuing without storage: %s",
                 esp_err_to_name(littlefs_result));
    }

    ESP_ERROR_CHECK(lcd_clear(LCD_COLOR_GREEN));
    uint32_t seconds = 0;
    char uptime_text[40];
    while (true) {
        snprintf(uptime_text,
                 sizeof(uptime_text),
                 "Hello World - uptime: %u s",
                 (unsigned)seconds);

        /* 每次刷新前清除第一行，避免较短的新内容后面残留旧字符。 */
        esp_err_t display_result = lcd_fill_rect(0,
                                                  0,
                                                  16,
                                                  LCD_Y_RESOLUTION,
                                                  LCD_COLOR_WHITE);
        if (display_result == ESP_OK) {
            display_result = lcd_show_string(0,
                                             0,
                                             16,
                                             LCD_Y_RESOLUTION,
                                             16,
                                             uptime_text,
                                             LCD_COLOR_BLACK);
        }
        if (display_result == ESP_OK) {
            display_result = lcd_show_string(20,
                                             0,
                                             16,
                                             LCD_Y_RESOLUTION,
                                             16,
                                             "xc_lcd",
                                             LCD_COLOR_BLACK);
        }
        if (display_result == ESP_OK) {
            display_result = lcd_show_char(40,
                                           0,
                                           'A',
                                           16,
                                           LCD_COLOR_BLACK);
        }
        if (display_result != ESP_OK) {
            ESP_LOGE(TAG,
                     "Failed to update LCD content: %s",
                     esp_err_to_name(display_result));
        }

        seconds += 1;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
