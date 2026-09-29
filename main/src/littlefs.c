#include "littlefs.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_littlefs.h"
#include "esp_log.h"

#define LITTLEFS_BASE_PATH "/littlefs"
#define LITTLEFS_PARTITION_LABEL "storage"
#define LITTLEFS_TEST_FILE_PATH LITTLEFS_BASE_PATH "/hello.txt"
#define LITTLEFS_RENAMED_FILE_PATH LITTLEFS_BASE_PATH "/foo.txt"
#define LITTLEFS_TEST_CONTENT "Hello Flash LittleFS from BOX3!\n"
#define LITTLEFS_TEST_BUFFER_SIZE 64U
#define LITTLEFS_BYTES_PER_MIB (1024U * 1024U)

static const char *TAG = "LITTLEFS";
static bool littlefs_mounted = false;

esp_err_t littlefs_init(void)
{
    if (littlefs_mounted) {
        return ESP_OK;
    }

    /*
     * LittleFS 使用内部 Flash 中名为 storage 的数据分区，挂载点为
     * /littlefs。分区首次使用或文件系统损坏时允许自动格式化，避免
     * 因为分区为空而导致整个应用重启。
     */
    const esp_vfs_littlefs_conf_t config = {
        .base_path = LITTLEFS_BASE_PATH,
        .partition_label = LITTLEFS_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t result = esp_vfs_littlefs_register(&config);
    if (result != ESP_OK) {
        if (result == ESP_FAIL) {
            ESP_LOGW(TAG, "Failed to mount or format LittleFS");
        } else if (result == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG,
                     "LittleFS partition '%s' was not found",
                     LITTLEFS_PARTITION_LABEL);
        } else {
            ESP_LOGW(TAG,
                     "Failed to initialize LittleFS: %s",
                     esp_err_to_name(result));
        }
        return result;
    }

    size_t total_bytes = 0;
    size_t used_bytes = 0;
    result = esp_littlefs_info(LITTLEFS_PARTITION_LABEL,
                               &total_bytes,
                               &used_bytes);
    if (result != ESP_OK) {
        ESP_LOGW(TAG,
                 "Failed to query LittleFS partition: %s",
                 esp_err_to_name(result));
        esp_vfs_littlefs_unregister(LITTLEFS_PARTITION_LABEL);
        return result;
    }

    littlefs_mounted = true;
    ESP_LOGI(TAG,
             "LittleFS mounted at %s: total=%u bytes, used=%u bytes",
             LITTLEFS_BASE_PATH,
             (unsigned int)total_bytes,
             (unsigned int)used_bytes);
    return ESP_OK;
}

esp_err_t littlefs_test(void)
{
    /* 只有 littlefs_init() 挂载成功后，/littlefs 路径才可以进行文件操作。 */
    if (!littlefs_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 读取 LittleFS 分区的总容量和已用容量。
     * 这一步既用于打印空间信息，也可以提前发现文件系统状态异常。
     */
    size_t total_bytes = 0;
    size_t used_bytes = 0;
    esp_err_t result = esp_littlefs_info(LITTLEFS_PARTITION_LABEL,
                                         &total_bytes,
                                         &used_bytes);
    if (result != ESP_OK) {
        ESP_LOGW(TAG,
                 "Failed to query LittleFS capacity; skip file test: %s",
                 esp_err_to_name(result));
        return result;
    }

    /* LittleFS 接口返回已用空间，因此用总容量减去已用容量得到剩余空间。 */
    const size_t free_bytes = total_bytes >= used_bytes
                                  ? total_bytes - used_bytes
                                  : 0U;
    ESP_LOGI(TAG,
             "LittleFS: total=%u MiB, free=%u MiB",
             (unsigned int)(total_bytes / LITTLEFS_BYTES_PER_MIB),
             (unsigned int)(free_bytes / LITTLEFS_BYTES_PER_MIB));

    /*
     * 以写入模式创建 hello.txt。
     * "w" 模式会创建新文件，或清空同名旧文件，保证本次测试内容固定。
     */
    FILE *file = fopen(LITTLEFS_TEST_FILE_PATH, "w");
    if (file == NULL) {
        ESP_LOGE(TAG,
                 "Failed to open %s for writing: errno=%d",
                 LITTLEFS_TEST_FILE_PATH,
                 errno);
        return ESP_FAIL;
    }

    /* 计算测试字符串长度，并把完整字符串写入文件。 */
    const size_t expected_size = strlen(LITTLEFS_TEST_CONTENT);
    const size_t written_size = fwrite(LITTLEFS_TEST_CONTENT,
                                       1,
                                       expected_size,
                                       file);

    /* 关闭文件会刷新缓存；写入字节数和关闭结果都必须正确。 */
    const int write_close_result = fclose(file);
    if (written_size != expected_size || write_close_result != 0) {
        ESP_LOGE(TAG, "Failed to write %s", LITTLEFS_TEST_FILE_PATH);
        return ESP_FAIL;
    }

    /* 重新以只读模式打开 hello.txt，验证刚才写入的内容。 */
    file = fopen(LITTLEFS_TEST_FILE_PATH, "r");
    if (file == NULL) {
        ESP_LOGE(TAG,
                 "Failed to open %s for reading: errno=%d",
                 LITTLEFS_TEST_FILE_PATH,
                 errno);
        return ESP_FAIL;
    }

    /* 预留一个字节放置字符串结束符，便于后面的日志按字符串输出。 */
    char read_buffer[LITTLEFS_TEST_BUFFER_SIZE] = {0};
    const size_t read_size = fread(read_buffer,
                                   1,
                                   sizeof(read_buffer) - 1U,
                                   file);

    /* 同时检查读取错误、关闭错误、读取长度以及实际内容是否完全一致。 */
    const int read_error = ferror(file);
    const int read_close_result = fclose(file);
    if (read_error != 0 ||
        read_close_result != 0 ||
        read_size != expected_size ||
        memcmp(read_buffer, LITTLEFS_TEST_CONTENT, expected_size) != 0) {
        ESP_LOGE(TAG,
                 "LittleFS read-back verification failed for %s",
                 LITTLEFS_TEST_FILE_PATH);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "LittleFS file test passed: %s, read=%u bytes, content=\"%s\"",
             LITTLEFS_TEST_FILE_PATH,
             (unsigned int)read_size,
             read_buffer);

    /*
     * 重命名之前先检查目标 foo.txt。
     * 如果目标已经存在，先删除它，避免 FATFS/LittleFS 因目标重名而失败。
     * stat() 失败时只有 ENOENT（目标不存在）属于正常情况。
     */
    struct stat file_status;
    if (stat(LITTLEFS_RENAMED_FILE_PATH, &file_status) == 0) {
        if (unlink(LITTLEFS_RENAMED_FILE_PATH) != 0) {
            ESP_LOGE(TAG,
                     "Failed to remove %s: errno=%d",
                     LITTLEFS_RENAMED_FILE_PATH,
                     errno);
            return ESP_FAIL;
        }
    } else if (errno != ENOENT) {
        ESP_LOGE(TAG,
                 "Failed to check %s: errno=%d",
                 LITTLEFS_RENAMED_FILE_PATH,
                 errno);
        return ESP_FAIL;
    }

    /* 将已完成读回校验的 hello.txt 重命名为 foo.txt。 */
    ESP_LOGI(TAG,
             "Renaming %s to %s",
             LITTLEFS_TEST_FILE_PATH,
             LITTLEFS_RENAMED_FILE_PATH);
    if (rename(LITTLEFS_TEST_FILE_PATH, LITTLEFS_RENAMED_FILE_PATH) != 0) {
        ESP_LOGE(TAG, "Rename failed: errno=%d", errno);
        return ESP_FAIL;
    }

    /* 到这里表示容量查询、写入、读回校验、删除和重命名全部成功。 */
    ESP_LOGI(TAG,
             "LittleFS rename test passed: %s",
             LITTLEFS_RENAMED_FILE_PATH);
    return ESP_OK;
}

esp_err_t littlefs_deinit(void)
{
    if (!littlefs_mounted) {
        return ESP_OK;
    }

    esp_err_t result = esp_vfs_littlefs_unregister(LITTLEFS_PARTITION_LABEL);
    if (result == ESP_OK) {
        littlefs_mounted = false;
    }
    return result;
}
