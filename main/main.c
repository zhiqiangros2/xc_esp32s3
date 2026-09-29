#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "aw9523b.h"
#include "bsp_info.h"
#include "display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c.h"
#include "interrupt_manager.h"
#include "key_interrupt.h"
#include "lcd.h"
#include "nvs_flash.h"
#include "sd.h"
#include "spi.h"
#include "tp.h"

#define BYTES_PER_MIB (1024U * 1024U)

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
     * SD_CS=GPIO18，ESP-IDF 会按设备片选自动串行化读写事务。
     */
    ESP_ERROR_CHECK(board_spi_init(BOARD_SPI_MAX_TRANSFER_SIZE));

    /*
     * 基础外设就绪后初始化 LCD，并显示八色竖向测试色条，用于检查屏幕
     * 刷新、RGB565 颜色以及横屏显示方向是否正常。
     */
    ESP_ERROR_CHECK(lcd_init());
    ESP_ERROR_CHECK(lcd_show_test_pattern());

    /*
     * SD 与 LCD 共用 SPI2，仅片选不同。未插卡、卡损坏或文件系统无法挂载时
     * 只记录警告，不触发 ESP_ERROR_CHECK 重启，其他功能继续正常运行。
     */
    esp_err_t sd_result = sd_init();
    if (sd_result != ESP_OK) {
        ESP_LOGW(TAG,
                 "SD unavailable; continuing without storage: %s",
                 esp_err_to_name(sd_result));
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
    /* 等待 1 秒后再读取 SD 卡容量，避免刚挂载完成就立即查询。 */
    if (sd_result == ESP_OK) {
        uint64_t sd_total_bytes = 0;
        uint64_t sd_free_bytes = 0;
        sd_result = sd_get_usage(&sd_total_bytes, &sd_free_bytes);
        if (sd_result == ESP_OK) {
            ESP_LOGI(TAG,
                     "SD FATFS: total=%llu MiB, free=%llu MiB",
                     (unsigned long long)(sd_total_bytes / BYTES_PER_MIB),
                     (unsigned long long)(sd_free_bytes / BYTES_PER_MIB));
        } else {
            ESP_LOGW(TAG,
                     "Failed to query SD capacity: %s",
                     esp_err_to_name(sd_result));
        }
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
