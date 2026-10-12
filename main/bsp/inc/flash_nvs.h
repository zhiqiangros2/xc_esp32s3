#ifndef BSP_FLASH_NVS_H
#define BSP_FLASH_NVS_H

#include <stddef.h>

#include "esp_err.h"

/** STA 与 SoftAP 的凭据分别使用独立键保存。 */
typedef enum {
    FLASH_NVS_WIFI_TARGET_STA,
    FLASH_NVS_WIFI_TARGET_AP,
} flash_nvs_wifi_target_t;

/**
 * @brief 从 NVS 读取一组 WiFi SSID 和密码。
 *
 * 本函数内部完成 nvs_open()、两个字符串的读取和 nvs_close()。命名空间或
 * 任意一个键尚不存在时清空两个输出缓冲区，并统一返回 ESP_ERR_NOT_FOUND。
 *
 * @param[in] target 读取 STA 或 SoftAP 凭据。
 * @param[out] ssid SSID 输出缓冲区。
 * @param[in] ssid_size SSID 输出缓冲区大小。
 * @param[out] password 密码输出缓冲区。
 * @param[in] password_size 密码输出缓冲区大小。
 */
esp_err_t flash_nvs_load_wifi_credentials(flash_nvs_wifi_target_t target,
                                          char *ssid,
                                          size_t ssid_size,
                                          char *password,
                                          size_t password_size);

/**
 * @brief 把一组 WiFi SSID 和密码写入 NVS。
 *
 * 两个字符串写入同一个已打开的命名空间，最后只调用一次 nvs_commit()；
 * 无论成功还是失败都会关闭 NVS 句柄。保存结果跨重启保留，运行期间的
 * STA 断线不会清除它；用户再次选择同类配置时才会覆盖原来的两个字符串。
 */
esp_err_t flash_nvs_save_wifi_credentials(flash_nvs_wifi_target_t target,
                                          const char *ssid,
                                          const char *password);

/**
 * @brief 删除 NVS 中保存的 STA 和 SoftAP SSID、密码。
 *
 * 四个键在同一次 nvs_commit() 中删除。命名空间尚不存在时视为已经清空，
 * 返回 ESP_OK。
 */
esp_err_t flash_nvs_clear_wifi_credentials(void);

#endif
