/**
 * @file flash_nvs.c
 * @brief WiFi STA/SoftAP 凭据的 NVS 持久化实现。
 *
 * 本文件是项目中 WiFi 配置访问 NVS 的唯一入口：
 *
 *   wifi_manager
 *        |
 *        | load/save/clear credentials
 *        v
 *   flash_nvs.c
 *        |
 *        | nvs_open -> nvs_get/set_str -> nvs_commit -> nvs_close
 *        v
 *   Flash NVS 分区
 *
 * WiFi 驱动不接触 NVS 句柄和键名。读取时要求 SSID 与密码同时存在；缺少
 * 任意一个键都按“尚未选择该配置”处理。清空时一次删除 STA/AP 四个键。
 */
#include "flash_nvs.h"

#include "nvs.h"

#define FLASH_NVS_WIFI_NAMESPACE "wifi_manager"
#define FLASH_NVS_WIFI_STA_SSID_KEY "sta_ssid"
#define FLASH_NVS_WIFI_STA_PASSWORD_KEY "sta_pass"
#define FLASH_NVS_WIFI_AP_SSID_KEY "ap_ssid"
#define FLASH_NVS_WIFI_AP_PASSWORD_KEY "ap_pass"

/** 根据保存目标返回对应的两个 NVS 键名。 */
static esp_err_t wifi_credential_keys(flash_nvs_wifi_target_t target,
                                      const char **ssid_key,
                                      const char **password_key)
{
    if (ssid_key == NULL || password_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (target) {
        case FLASH_NVS_WIFI_TARGET_STA:
            *ssid_key = FLASH_NVS_WIFI_STA_SSID_KEY;
            *password_key = FLASH_NVS_WIFI_STA_PASSWORD_KEY;
            return ESP_OK;
        case FLASH_NVS_WIFI_TARGET_AP:
            *ssid_key = FLASH_NVS_WIFI_AP_SSID_KEY;
            *password_key = FLASH_NVS_WIFI_AP_PASSWORD_KEY;
            return ESP_OK;
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t flash_nvs_load_wifi_credentials(flash_nvs_wifi_target_t target,
                                          char *ssid,
                                          size_t ssid_size,
                                          char *password,
                                          size_t password_size)
{
    if (ssid == NULL || ssid_size == 0U ||
        password == NULL || password_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    ssid[0] = '\0';
    password[0] = '\0';

    const char *ssid_key;
    const char *password_key;
    esp_err_t result = wifi_credential_keys(target,
                                            &ssid_key,
                                            &password_key);
    if (result != ESP_OK) {
        return result;
    }

    nvs_handle_t nvs;
    result = nvs_open(FLASH_NVS_WIFI_NAMESPACE, NVS_READONLY, &nvs);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (result != ESP_OK) {
        return result;
    }

    size_t stored_ssid_size = ssid_size;
    result = nvs_get_str(nvs, ssid_key, ssid, &stored_ssid_size);
    if (result == ESP_OK) {
        size_t stored_password_size = password_size;
        result = nvs_get_str(nvs,
                             password_key,
                             password,
                             &stored_password_size);
    }
    nvs_close(nvs);

    if (result == ESP_ERR_NVS_NOT_FOUND) {
        result = ESP_ERR_NOT_FOUND;
    }
    if (result != ESP_OK) {
        ssid[0] = '\0';
        password[0] = '\0';
    }
    return result;
}

esp_err_t flash_nvs_save_wifi_credentials(flash_nvs_wifi_target_t target,
                                          const char *ssid,
                                          const char *password)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *ssid_key;
    const char *password_key;
    esp_err_t result = wifi_credential_keys(target,
                                            &ssid_key,
                                            &password_key);
    if (result != ESP_OK) {
        return result;
    }

    nvs_handle_t nvs;
    result = nvs_open(FLASH_NVS_WIFI_NAMESPACE, NVS_READWRITE, &nvs);
    if (result != ESP_OK) {
        return result;
    }

    result = nvs_set_str(nvs, ssid_key, ssid);
    if (result == ESP_OK) {
        result = nvs_set_str(nvs, password_key, password);
    }
    if (result == ESP_OK) {
        result = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return result;
}

esp_err_t flash_nvs_clear_wifi_credentials(void)
{
    nvs_handle_t nvs;
    esp_err_t result = nvs_open(FLASH_NVS_WIFI_NAMESPACE,
                                NVS_READWRITE,
                                &nvs);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (result != ESP_OK) {
        return result;
    }

    /* 该命名空间只保存两组 WiFi 凭据，一次清空并只提交一次 Flash 写入。 */
    result = nvs_erase_all(nvs);
    if (result == ESP_OK) {
        result = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return result;
}
