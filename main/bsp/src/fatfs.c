#include "fatfs.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"

#define FATFS_BASE_PATH "/vfs"
#define FATFS_PARTITION_LABEL "vfs"
#define FATFS_MAX_OPEN_FILES 5
#define FATFS_ALLOCATION_UNIT_SIZE 4096U
#define FATFS_TEST_FILE_PATH FATFS_BASE_PATH "/hello.txt"
#define FATFS_RENAMED_FILE_PATH FATFS_BASE_PATH "/foo.txt"
#define FATFS_TEST_CONTENT "Hello Flash FATFS from BOX3!\n"
#define FATFS_TEST_BUFFER_SIZE 64U
#define FATFS_BYTES_PER_MIB (1024U * 1024U)

static const char *TAG = "FATFS";
static bool fatfs_mounted = false;
static wl_handle_t fatfs_wl_handle = WL_INVALID_HANDLE;

esp_err_t fatfs_init(void)
{
    if (fatfs_mounted) {
        return ESP_OK;
    }

    /*
     * vfs 是内部 Flash 上的 FAT 分区。读写挂载必须配合 Wear Levelling，
     * 这样 FATFS 的扇区访问才能转换为适合 Flash 的擦写操作。
     * 第一次使用时分区通常还没有 FAT 格式，因此允许挂载失败后自动格式化。
    */
    const esp_vfs_fat_mount_config_t mount_config = {
        /* 分区没有有效 FAT 格式时自动格式化，首次使用内部 Flash 时需要开启。 */
        .format_if_mount_failed = true,
        /* FATFS 同时允许打开的最大文件数，包含测试文件使用的文件句柄。 */
        .max_files = FATFS_MAX_OPEN_FILES,
        /* FATFS 分配单元大小为 4096 字节，影响空间利用率和读写性能。 */
        .allocation_unit_size = FATFS_ALLOCATION_UNIT_SIZE,
    };

    esp_err_t result = esp_vfs_fat_spiflash_mount_rw_wl(FATFS_BASE_PATH,
                                                        FATFS_PARTITION_LABEL,
                                                        &mount_config,
                                                        &fatfs_wl_handle);
    if (result != ESP_OK) {
        fatfs_wl_handle = WL_INVALID_HANDLE;
        ESP_LOGW(TAG,
                 "Failed to mount Flash FATFS partition '%s': %s",
                 FATFS_PARTITION_LABEL,
                 esp_err_to_name(result));
        return result;
    }

    fatfs_mounted = true;
    ESP_LOGI(TAG,
             "Flash FATFS mounted at %s: partition=%s",
             FATFS_BASE_PATH,
             FATFS_PARTITION_LABEL);

    /* 挂载成功后直接读取 FATFS 容量，确认文件系统可以正常访问。 */
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    const esp_err_t info_result = esp_vfs_fat_info(FATFS_BASE_PATH,
                                                   &total_bytes,
                                                   &free_bytes);
    if (info_result == ESP_OK) {
        ESP_LOGI(TAG,
                 "Flash FATFS: total=%llu MiB, free=%llu MiB",
                 (unsigned long long)(total_bytes / FATFS_BYTES_PER_MIB),
                 (unsigned long long)(free_bytes / FATFS_BYTES_PER_MIB));
    } else {
        ESP_LOGW(TAG,
                 "Failed to query Flash FATFS capacity: %s",
                 esp_err_to_name(info_result));
    }

    return ESP_OK;
}

esp_err_t fatfs_test(void)
{
    /* 未成功挂载时，/vfs 路径不能进行文件操作。 */
    if (!fatfs_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 测试开始前再次查询容量，确认当前文件系统仍然可用。 */
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    esp_err_t result = esp_vfs_fat_info(FATFS_BASE_PATH,
                                        &total_bytes,
                                        &free_bytes);
    if (result != ESP_OK) {
        ESP_LOGW(TAG,
                 "Failed to query Flash FATFS capacity; skip file test: %s",
                 esp_err_to_name(result));
        return result;
    }

    ESP_LOGI(TAG,
             "Flash FATFS test space: total=%llu MiB, free=%llu MiB",
             (unsigned long long)(total_bytes / FATFS_BYTES_PER_MIB),
             (unsigned long long)(free_bytes / FATFS_BYTES_PER_MIB));

    /* 使用写入模式创建 hello.txt；每次测试都会清空旧内容。 */
    FILE *file = fopen(FATFS_TEST_FILE_PATH, "w");
    if (file == NULL) {
        ESP_LOGE(TAG,
                 "Failed to open %s for writing: errno=%d",
                 FATFS_TEST_FILE_PATH,
                 errno);
        return ESP_FAIL;
    }

    const size_t expected_size = strlen(FATFS_TEST_CONTENT);
    const size_t written_size = fwrite(FATFS_TEST_CONTENT,
                                       1,
                                       expected_size,
                                       file);
    const int write_close_result = fclose(file);
    if (written_size != expected_size || write_close_result != 0) {
        ESP_LOGE(TAG, "Failed to write %s", FATFS_TEST_FILE_PATH);
        return ESP_FAIL;
    }

    /* 重新打开原文件，读回并校验本次写入的完整内容。 */
    file = fopen(FATFS_TEST_FILE_PATH, "r");
    if (file == NULL) {
        ESP_LOGE(TAG,
                 "Failed to open %s for reading: errno=%d",
                 FATFS_TEST_FILE_PATH,
                 errno);
        return ESP_FAIL;
    }

    char read_buffer[FATFS_TEST_BUFFER_SIZE] = {0};
    const size_t read_size = fread(read_buffer,
                                   1,
                                   sizeof(read_buffer) - 1U,
                                   file);
    const int read_error = ferror(file);
    const int read_close_result = fclose(file);
    if (read_error != 0 ||
        read_close_result != 0 ||
        read_size != expected_size ||
        memcmp(read_buffer, FATFS_TEST_CONTENT, expected_size) != 0) {
        ESP_LOGE(TAG,
                 "Flash FATFS read-back verification failed for %s",
                 FATFS_TEST_FILE_PATH);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "Flash FATFS file test passed: %s, read=%u bytes, content=\"%s\"",
             FATFS_TEST_FILE_PATH,
             (unsigned int)read_size,
             read_buffer);

    /* 重命名之前先删除可能已经存在的目标文件。 */
    struct stat file_status;
    if (stat(FATFS_RENAMED_FILE_PATH, &file_status) == 0) {
        if (unlink(FATFS_RENAMED_FILE_PATH) != 0) {
            ESP_LOGE(TAG,
                     "Failed to remove existing %s: errno=%d",
                     FATFS_RENAMED_FILE_PATH,
                     errno);
            return ESP_FAIL;
        }
    } else if (errno != ENOENT) {
        ESP_LOGE(TAG,
                 "Failed to check %s: errno=%d",
                 FATFS_RENAMED_FILE_PATH,
                 errno);
        return ESP_FAIL;
    }

    /* 将已完成读写校验的文件重命名为 foo.txt。 */
    ESP_LOGI(TAG,
             "Renaming %s to %s",
             FATFS_TEST_FILE_PATH,
             FATFS_RENAMED_FILE_PATH);
    if (rename(FATFS_TEST_FILE_PATH, FATFS_RENAMED_FILE_PATH) != 0) {
        ESP_LOGE(TAG, "Rename failed: errno=%d", errno);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "Flash FATFS rename test passed: %s",
             FATFS_RENAMED_FILE_PATH);
    return ESP_OK;
}

esp_err_t fatfs_deinit(void)
{
    if (!fatfs_mounted) {
        return ESP_OK;
    }

    esp_err_t result = esp_vfs_fat_spiflash_unmount_rw_wl(FATFS_BASE_PATH,
                                                          fatfs_wl_handle);
    if (result == ESP_OK) {
        fatfs_mounted = false;
        fatfs_wl_handle = WL_INVALID_HANDLE;
    }
    return result;
}
