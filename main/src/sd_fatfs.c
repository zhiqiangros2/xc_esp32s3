#include "sd_fatfs.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "spi.h"

#define SD_CS_GPIO GPIO_NUM_18
#define SD_MAX_OPEN_FILES 5
#define SD_ALLOCATION_UNIT_SIZE 4096U
#define SD_FATFS_TEST_FILE_PATH SD_MOUNT_POINT "/hello.txt"
#define SD_FATFS_RENAMED_FILE_PATH SD_MOUNT_POINT "/foo.txt"
#define SD_FATFS_TEST_CONTENT "Hello SD FATFS from BOX3!\n"
#define SD_FATFS_TEST_BUFFER_SIZE 64U
#define SD_FATFS_BYTES_PER_MIB (1024U * 1024U)

static const char *TAG = "SD";
static const char *FATFS_TAG = "SD_FATFS";
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
    /*
     * 输出 SD 卡的识别信息和容量参数，方便确认卡片已经正确挂载：
     *
        Name: SL32G
        Type: SDHC
        Speed: 20.00 MHz (limit: 20.00 MHz)
        Size: 30436MB
        CSD: ver=2, sector_size=512, capacity=62333952 read_bl_len=9
        SSR: bus_width=1
     *
     * - Name：卡片厂商或型号名称，例如 SL32G。
     * - Type：卡片类型，例如 SDHC，表示高容量 SD 卡。
     * - Speed：当前 SPI 总线速度和卡片支持的速度上限，本工程为 20 MHz。
     * - Size：卡片可用的总容量，日志中的单位是 MB。
     * - CSD：卡片规格寄存器，包括版本、扇区大小、总扇区数和读块长度。
     *   read_bl_len=9  =>  2^9 = 512 字节  总容量 ≈ 62333952 × 512 字节
     * - SSR：SD 状态寄存器；bus_width=1 表示当前 SPI 模式使用单线数据通道。
     *
     * 该函数只打印信息，不会修改卡内数据。
    */
    sdmmc_card_print_info(stdout, sd_card);

    /* 挂载成功后直接查询 FATFS 容量，确认文件系统空间可正常访问。 */
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    const esp_err_t info_result = esp_vfs_fat_info(SD_MOUNT_POINT,
                                                   &total_bytes,
                                                   &free_bytes);
    if (info_result == ESP_OK) {
        ESP_LOGI(TAG,
                 "FATFS: total=%llu MiB, free=%llu MiB",
                 (unsigned long long)(total_bytes / SD_FATFS_BYTES_PER_MIB),
                 (unsigned long long)(free_bytes / SD_FATFS_BYTES_PER_MIB));
    } else {
        ESP_LOGW(TAG,
                 "Failed to query FATFS capacity: %s",
                 esp_err_to_name(info_result));
    }

    return ESP_OK;
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

esp_err_t sd_fatfs_test(void)
{
    if (sd_card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    esp_err_t result = esp_vfs_fat_info(SD_MOUNT_POINT,
                                        &total_bytes,
                                        &free_bytes);
    if (result != ESP_OK) {
        ESP_LOGW(FATFS_TAG,
                 "Failed to query SD capacity; skip FATFS file test: %s",
                 esp_err_to_name(result));
        return result;
    }

    ESP_LOGI(FATFS_TAG,
             "SD FATFS: total=%llu MiB, free=%llu MiB",
             (unsigned long long)(total_bytes / SD_FATFS_BYTES_PER_MIB),
             (unsigned long long)(free_bytes / SD_FATFS_BYTES_PER_MIB));

    /*
     * 使用写入模式创建测试文件。每次打开都会清空旧内容，确保后面的
     * 读回校验只检查本次测试写入的数据。
     */
    FILE *file = fopen(SD_FATFS_TEST_FILE_PATH, "w");
    if (file == NULL) {
        ESP_LOGE(FATFS_TAG,
                 "Failed to open %s for writing",
                 SD_FATFS_TEST_FILE_PATH);
        return ESP_FAIL;
    }

    const size_t expected_size = strlen(SD_FATFS_TEST_CONTENT);
    const size_t written_size = fwrite(SD_FATFS_TEST_CONTENT,
                                       1,
                                       expected_size,
                                       file);
    const int write_close_result = fclose(file);
    if (written_size != expected_size || write_close_result != 0) {
        ESP_LOGE(FATFS_TAG,
                 "Failed to write %s",
                 SD_FATFS_TEST_FILE_PATH);
        return ESP_FAIL;
    }

    /* 重新打开文件，读取刚才写入的内容并逐字节比较。 */
    file = fopen(SD_FATFS_TEST_FILE_PATH, "r");
    if (file == NULL) {
        ESP_LOGE(FATFS_TAG,
                 "Failed to open %s for reading",
                 SD_FATFS_TEST_FILE_PATH);
        return ESP_FAIL;
    }

    char read_buffer[SD_FATFS_TEST_BUFFER_SIZE] = {0};
    const size_t read_size = fread(read_buffer,
                                   1,
                                   sizeof(read_buffer) - 1U,
                                   file);
    const int read_error = ferror(file);
    const int read_close_result = fclose(file);
    if (read_error != 0 ||
        read_close_result != 0 ||
        read_size != expected_size ||
        memcmp(read_buffer, SD_FATFS_TEST_CONTENT, expected_size) != 0) {
        ESP_LOGE(FATFS_TAG,
                 "FATFS read-back verification failed for %s",
                 SD_FATFS_TEST_FILE_PATH);
        return ESP_FAIL;
    }

    ESP_LOGI(FATFS_TAG,
             "FATFS file test passed: %s, read=%u bytes, content=\"%s\"",
             SD_FATFS_TEST_FILE_PATH,
             (unsigned int)read_size,
             read_buffer);

    /* 重命名之前先删除可能已经存在的目标文件。 */
    struct stat file_status;
    if (stat(SD_FATFS_RENAMED_FILE_PATH, &file_status) == 0) {
        if (unlink(SD_FATFS_RENAMED_FILE_PATH) != 0) {
            ESP_LOGE(FATFS_TAG,
                     "Failed to remove existing %s: errno=%d",
                     SD_FATFS_RENAMED_FILE_PATH,
                     errno);
            return ESP_FAIL;
        }
    } else if (errno != ENOENT) {
        ESP_LOGE(FATFS_TAG,
                 "Failed to check %s: errno=%d",
                 SD_FATFS_RENAMED_FILE_PATH,
                 errno);
        return ESP_FAIL;
    }

    /* 将已完成读写校验的文件重命名为 foo.txt。 */
    ESP_LOGI(FATFS_TAG,
             "Renaming %s to %s",
             SD_FATFS_TEST_FILE_PATH,
             SD_FATFS_RENAMED_FILE_PATH);
    if (rename(SD_FATFS_TEST_FILE_PATH, SD_FATFS_RENAMED_FILE_PATH) != 0) {
        ESP_LOGE(FATFS_TAG,
                 "Rename failed: errno=%d",
                 errno);
        return ESP_FAIL;
    }

    ESP_LOGI(FATFS_TAG,
             "FATFS rename test passed: %s",
             SD_FATFS_RENAMED_FILE_PATH);
    return ESP_OK;
}
