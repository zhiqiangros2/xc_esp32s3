#include "bsp_info.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"

#define BSP_BYTES_PER_MIB (1024U * 1024U)

static const char *TAG = "BOX3";

/* 这些符号由 ESP-IDF 链接脚本提供，分别表示 Flash 映射代码和只读数据范围。 */
extern const uint8_t _text_start[];
extern const uint8_t _text_end[];
extern const uint8_t _rodata_start[];
extern const uint8_t _rodata_end[];

static size_t bsp_linker_region_size(const uint8_t *start,
                                     const uint8_t *end)
{
    const uintptr_t start_address = (uintptr_t)start;
    const uintptr_t end_address = (uintptr_t)end;
    return end_address > start_address ? end_address - start_address : 0;
}

static bool bsp_test_psram(void)
{
    uint8_t *buffer = heap_caps_malloc(BSP_BYTES_PER_MIB,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate 1 MiB from PSRAM");
        return false;
    }

    for (size_t i = 0; i < BSP_BYTES_PER_MIB; ++i) {
        buffer[i] = (uint8_t)((i * 31U) ^ (i >> 8));
    }

    for (size_t i = 0; i < BSP_BYTES_PER_MIB; ++i) {
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

void bsp_info_print(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;

    esp_chip_info(&chip_info);

    printf("\nHello World from ALIENTEK DNESP32S3 BOX3!\n");
    printf("ESP-IDF: %s\n", esp_get_idf_version());
    printf("Chip: %s, %u CPU core(s), %s%s%s%s\n",
           CONFIG_IDF_TARGET,
           chip_info.cores,
           (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
           (chip_info.features & CHIP_FEATURE_BT) ? "BT/" : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
           (chip_info.features & CHIP_FEATURE_IEEE802154)
               ? ", 802.15.4 (Zigbee/Thread)"
               : "");

    const unsigned major_revision = chip_info.revision / 100U;
    const unsigned minor_revision = chip_info.revision % 100U;
    printf("Silicon revision: v%u.%u\n", major_revision, minor_revision);

    const esp_err_t flash_result = esp_flash_get_size(NULL, &flash_size);
    if (flash_result != ESP_OK) {
        printf("Flash: get size failed: %s\n", esp_err_to_name(flash_result));
    } else {
        printf("Flash: %u MiB, %s\n",
               (unsigned)(flash_size / BSP_BYTES_PER_MIB),
               (chip_info.features & CHIP_FEATURE_EMB_FLASH)
                   ? "embedded"
                   : "external");
    }

    const size_t psram_size =
        esp_psram_is_initialized() ? esp_psram_get_size() : 0;
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const size_t iram_total = heap_caps_get_total_size(MALLOC_CAP_EXEC);
    const size_t iram_free = heap_caps_get_free_size(MALLOC_CAP_EXEC);
    const uint32_t dram_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t dram_total = heap_caps_get_total_size(dram_caps);
    const size_t dram_free = heap_caps_get_free_size(dram_caps);
    const size_t irom_used = bsp_linker_region_size(_text_start, _text_end);
    const size_t drom_used = bsp_linker_region_size(_rodata_start, _rodata_end);

    printf("Memory:\n");
    printf("  IROM: %u KiB used (Flash-mapped code)\n",
           (unsigned)(irom_used / 1024U));
    printf("  DROM: %u KiB used (Flash-mapped read-only data)\n",
           (unsigned)(drom_used / 1024U));
    printf("  IRAM: %u KiB total, %u KiB free\n",
           (unsigned)(iram_total / 1024U),
           (unsigned)(iram_free / 1024U));
    printf("  DRAM: %u KiB total, %u KiB free\n",
           (unsigned)(dram_total / 1024U),
           (unsigned)(dram_free / 1024U));
    printf("  PSRAM: %u MiB physical, %u KiB heap total, %u KiB free\n",
           (unsigned)(psram_size / BSP_BYTES_PER_MIB),
           (unsigned)(psram_total / 1024U),
           (unsigned)(psram_free / 1024U));
    printf("Minimum free heap: %u bytes\n",
           (unsigned)esp_get_minimum_free_heap_size());

    if (flash_result == ESP_OK && flash_size != 16U * BSP_BYTES_PER_MIB) {
        ESP_LOGW(TAG,
                 "Expected 16 MiB Flash, detected %u bytes",
                 (unsigned)flash_size);
    }
    if (psram_size != 8U * BSP_BYTES_PER_MIB) {
        ESP_LOGW(TAG,
                 "Expected 8 MiB PSRAM, detected %u bytes",
                 (unsigned)psram_size);
    }

    ESP_LOGI(TAG,
             "1 MiB PSRAM read/write test: %s",
             bsp_test_psram() ? "PASS" : "FAIL");
}
