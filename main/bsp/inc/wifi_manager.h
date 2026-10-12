#ifndef BSP_WIFI_MANAGER_H
#define BSP_WIFI_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* STA 和 SoftAP 各提供 3 组固定配置，UI 只传配置序号，不接触密码。 */
#define WIFI_MANAGER_PROFILE_COUNT 3U
#define WIFI_MANAGER_SSID_MAX_LENGTH 32U
#define WIFI_MANAGER_IPV4_TEXT_LENGTH 16U

/**
 * WiFi 当前状态的一致性快照，可由 LVGL 或其他业务任务读取。
 * configured 表示 Flash 中已有选择；connected/started 表示本次运行的实时
 * 状态，两类状态不能混为一谈。例如 STA 断线重连时 sta_configured 仍为 true，
 * sta_connected 暂时为 false，sta_reconnecting 为 true。
 */
typedef struct {
    bool ready;

    bool sta_configured;
    bool sta_connected;
    bool sta_reconnecting;
    char sta_ssid[WIFI_MANAGER_SSID_MAX_LENGTH + 1U];
    char sta_ip[WIFI_MANAGER_IPV4_TEXT_LENGTH];

    bool ap_configured;
    bool ap_started;
    bool napt_enabled;
    uint8_t ap_client_count;
    char ap_ssid[WIFI_MANAGER_SSID_MAX_LENGTH + 1U];
    char ap_ip[WIFI_MANAGER_IPV4_TEXT_LENGTH];
} wifi_manager_status_t;

/**
 * @brief 初始化 STA + SoftAP 共存服务。
 *
 * 必须在 nvs_flash_init() 之后调用。函数从 NVS 分别读取上次选择的 STA 和
 * SoftAP；存在任意一项时自动恢复，二者都存在时使用 WIFI_MODE_APSTA。
 * STA 断线后按“立即、2 秒、5 秒、每 10 秒”持续自动重连，避免上级热点
 * 长时间不存在时连续扫描影响 SoftAP 和 BLE。SoftAP 页面关闭后仍继续工作。
 *
 * 启动结果：
 * - 首次开机或 NVS 已擦除：保持 idle，等待 UI 选择，不开启 STA/AP。
 * - 只保存过 STA：启动 STA 并自动连接保存的外网。
 * - 只保存过 AP：启动保存的 SoftAP，等待客户端连接。
 * - STA、AP 都保存过：启动 APSTA；AP 保持接入，STA 自动连接外网。
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief 异步选择同一序号的 STA 和 SoftAP 配置。
 *
 * 函数只把序号放入 BSP 队列并立即返回；内部 RAM 栈任务随后保存 NVS、
 * 连接 STA 并启动 SoftAP。这样允许 PSRAM 栈上的 LVGL 任务安全调用，因为
 * 真正会关闭 Flash Cache 的 NVS 写入不在调用者任务中执行。连续提交多个
 * 选择时，长度为 1 的队列保留尚未执行的最新选择。
 */
esp_err_t wifi_manager_select_profile(size_t profile_index);

/**
 * @brief 异步删除保存的 STA/AP 配置并停止当前 WiFi。
 *
 * 函数只提交队列命令；内部 RAM 栈任务删除 NVS 中两组 SSID/密码，停止 STA
 * 与 SoftAP，并把状态恢复为“未选择”。下次启动不会自动连接或开启热点。
 */
esp_err_t wifi_manager_clear_profiles(void);

/** 取得 STA 固定配置的 SSID；序号无效时返回 NULL。 */
const char *wifi_manager_get_sta_profile_ssid(size_t profile_index);

/** 取得 STA 固定配置的密码，供配置页面显示；序号无效时返回 NULL。 */
const char *wifi_manager_get_sta_profile_password(size_t profile_index);

/** 取得 SoftAP 固定配置的 SSID；序号无效时返回 NULL。 */
const char *wifi_manager_get_ap_profile_ssid(size_t profile_index);

/** 取得 SoftAP 固定配置的密码，供配置页面显示；序号无效时返回 NULL。 */
const char *wifi_manager_get_ap_profile_password(size_t profile_index);

/** 线程安全复制 WiFi 状态快照。 */
esp_err_t wifi_manager_get_status(wifi_manager_status_t *status);

#endif
