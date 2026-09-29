#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "aw9523b.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c.h"
#include "interrupt_manager.h"
#include "key_interrupt.h"
#include "lcd.h"
#include "tp.h"

#define BYTES_PER_MIB (1024U * 1024U)

static const char *TAG = "BOX3";
static bool test_psram(void)
{
    uint8_t *buffer = heap_caps_malloc(BYTES_PER_MIB,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate 1 MiB from PSRAM");
        return false;
    }

    for (size_t i = 0; i < BYTES_PER_MIB; ++i) {
        buffer[i] = (uint8_t)((i * 31U) ^ (i >> 8));
    }

    for (size_t i = 0; i < BYTES_PER_MIB; ++i) {
        const uint8_t expected = (uint8_t)((i * 31U) ^ (i >> 8));
        if (buffer[i] != expected) {
            ESP_LOGE(TAG, "PSRAM verify failed at offset %u", (unsigned)i);
            heap_caps_free(buffer);
            return false;
        }
    }

    heap_caps_free(buffer);
    return true;
}

void app_main(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;

    esp_chip_info(&chip_info);
    ESP_ERROR_CHECK(esp_flash_get_size(NULL, &flash_size));

    const size_t psram_size = esp_psram_is_initialized() ? esp_psram_get_size() : 0;
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    printf("\nHello World from ALIENTEK DNESP32S3 BOX3!\n");
    printf("ESP-IDF: %s\n", esp_get_idf_version());
    printf("Chip: %s, %u core(s), revision %u\n",
           CONFIG_IDF_TARGET,
           chip_info.cores,
           (unsigned)chip_info.revision);
    printf("Flash: %u MiB\n", (unsigned)(flash_size / BYTES_PER_MIB));
    printf("PSRAM: %u MiB total, %u KiB free\n",
           (unsigned)(psram_size / BYTES_PER_MIB),
           (unsigned)(psram_free / 1024U));

    if (flash_size != 16U * BYTES_PER_MIB) {
        ESP_LOGW(TAG, "Expected 16 MiB Flash, detected %u bytes", (unsigned)flash_size);
    }
    if (psram_size != 8U * BYTES_PER_MIB) {
        ESP_LOGW(TAG, "Expected 8 MiB PSRAM, detected %u bytes", (unsigned)psram_size);
    }

    ESP_LOGI(TAG, "1 MiB PSRAM read/write test: %s", test_psram() ? "PASS" : "FAIL");

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

    /*
     * 基础外设就绪后初始化 LCD，并显示八色竖向测试色条，用于检查屏幕
     * 刷新、RGB565 颜色以及横屏显示方向是否正常。
     */
    ESP_ERROR_CHECK(lcd_init());
    ESP_ERROR_CHECK(lcd_show_test_pattern());

    /* CHSC5432 通过 I2C 读取触点，复位信号由 AW9523B P1_7 控制。 */
    ESP_ERROR_CHECK(tp_init());

    /* GPIO42 触发后依次查询 AW9523B 和 CHSC5432，判断实际中断来源。 */
    ESP_ERROR_CHECK(interrupt_manager_init(INTERRUPT_SOURCE_AW9523B |
                                           INTERRUPT_SOURCE_TOUCH));


    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_ERROR_CHECK(lcd_clear(LCD_COLOR_LGRAY));
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
                                                  LCD_COLOR_BLACK);
        if (display_result == ESP_OK) {
            display_result = lcd_show_string(0,
                                             0,
                                             16,
                                             LCD_Y_RESOLUTION,
                                             16,
                                             uptime_text,
                                             LCD_COLOR_WHITE,
                                             LCD_COLOR_BLACK);
        }
        if (display_result == ESP_OK) {
            display_result = lcd_show_string(20,
                                             0,
                                             16,
                                             LCD_Y_RESOLUTION,
                                             16,
                                             "xc_lcd",
                                             LCD_COLOR_WHITE,
                                             LCD_COLOR_BLACK);
        }
        if (display_result != ESP_OK) {
            ESP_LOGE(TAG,
                     "Failed to update LCD uptime: %s",
                     esp_err_to_name(display_result));
        }

        seconds += 1;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
