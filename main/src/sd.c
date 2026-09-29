#include "sd.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "spi.h"

#define SD_CS_GPIO GPIO_NUM_18
#define SD_MAX_OPEN_FILES 5
#define SD_ALLOCATION_UNIT_SIZE 4096U

static const char *TAG = "SD";
static sdmmc_card_t *sd_card = NULL;

esp_err_t sd_init(void)
{
    if (sd_card != NULL) {
        return ESP_OK;
    }

    /* SPI2 由 main.c 统一初始化；这里仅把 SD 设备加入共享总线。 */
    esp_err_t result;
    /* SDSPI_HOST_DEFAULT() 使用 SPI 模式和 20 MHz 最高时钟。 */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = board_spi_get_host();

    sdspi_device_config_t device_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    device_config.host_id = board_spi_get_host();
    device_config.gpio_cs = SD_CS_GPIO;
    device_config.gpio_cd = SDSPI_SLOT_NO_CD;
    device_config.gpio_wp = SDSPI_SLOT_NO_WP;
    device_config.gpio_int = SDSPI_SLOT_NO_INT;

    const esp_vfs_fat_mount_config_t mount_config = {
        /* 挂载失败时绝不自动格式化，避免破坏卡中已有数据。 */
        .format_if_mount_failed = false,
        /* FAT 文件系统同时允许打开的最大文件数。 */
        .max_files = SD_MAX_OPEN_FILES,
        /* FAT 文件系统的分配单元大小，使用 4096 字节。 */
        .allocation_unit_size = SD_ALLOCATION_UNIT_SIZE,
    };

    result = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT,
                                     &host,
                                     &device_config,
                                     &mount_config,
                                     &sd_card);
    if (result != ESP_OK) {
        sd_card = NULL;
        ESP_LOGW(TAG,
                 "Card mount failed on CS GPIO%d: %s",
                 SD_CS_GPIO,
                 esp_err_to_name(result));
        return result;
    }

    ESP_LOGI(TAG,
             "Card mounted at %s: SPI2 20 MHz, CS=GPIO%d",
             SD_MOUNT_POINT,
             SD_CS_GPIO);
    sdmmc_card_print_info(stdout, sd_card);
    return ESP_OK;
}

esp_err_t sd_get_usage(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (sd_card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (total_bytes == NULL || free_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return esp_vfs_fat_info(SD_MOUNT_POINT, total_bytes, free_bytes);
}

esp_err_t sd_deinit(void)
{
    if (sd_card == NULL) {
        return ESP_OK;
    }

    esp_err_t result = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, sd_card);
    if (result != ESP_OK) {
        return result;
    }

    sd_card = NULL;
    return ESP_OK;
}
