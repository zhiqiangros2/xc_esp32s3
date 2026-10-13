/**
 * @file wifi_manager.c
 * @brief STA 外网、SoftAP 热点、NVS 配置保存和 NAPT 共享上网。
 *
 * 数据路径：
 *
 *   上级路由器/手机热点
 *           ^
 *           | STA：tom123 / tom1234 / tom12345
 *           | 获取外网 IP、网关和 DNS
 *           v
 *        ESP32-S3
 *           |
 *           | IPv4 转发 + NAPT
 *           | 把 AP 客户端的私网地址转换为 STA 地址
 *           v
 *   SoftAP：ap_tom123 / ap_tom1234 / ap_tom12345
 *           |
 *           v
 *       手机、电脑等客户端
 *
 * STA 与 SoftAP 使用同一套 2.4 GHz WiFi 硬件。两项都已选择时驱动工作在
 * WIFI_MODE_APSTA：STA 负责外网，SoftAP 负责接入。STA 连上路由器后，
 * SoftAP 会跟随 STA 所在信道，这是单射频 APSTA 的正常行为。
 *
 * UI 只把配置序号放入队列并读取状态快照。BSP 内部任务使用内部 RAM 栈，
 * 负责保存 Flash NVS 和重配 WiFi；即使 LVGL 任务栈位于 PSRAM，Flash 写入
 * 期间关闭 Cache 也不会访问外部栈。页面关闭不会停止网络。
 * esp_wifi_set_storage(WIFI_STORAGE_RAM) 用来禁止 WiFi 驱动再保存一份配置，
 * Flash 中只有 flash_nvs 模块维护的一份明确配置。
 *
 * 四个典型阶段的现象：
 *
 * 1. 第一次开机（NVS 中还没有 STA、AP 配置）
 *
 *      初始化网卡、事件循环和 WiFi 驱动
 *                       |
 *                       v
 *      不调用 esp_wifi_start()，不连接 STA，也不开启 SoftAP
 *                       |
 *                       v
 *      UI 显示 STA、AP 均未选择，等待用户分别点击配置按钮
 *
 * 2. 用户选择 STA 和 AP 后
 *
 *      选择 STA -> 先保存 STA 账号到 NVS -> 启动/重配 STA -> 连接外网
 *      选择 AP  -> 先保存 AP 账号到 NVS  -> 启动/重配 AP
 *      AP 已启动且 STA 获得 IP -> 更新 DHCP DNS -> 开启 NAPT
 *
 *    两项选择顺序不受限制。只选一项时分别运行 WIFI_MODE_STA 或
 *    WIFI_MODE_AP；两项都已选择时运行 WIFI_MODE_APSTA。STA 获得 IP 后，
 *    SoftAP DHCP 会同步上级 DNS，连接到 SoftAP 的设备便可通过 STA 上网。
 *
 * 3. 运行期间 STA 断开
 *
 *      清除 sta_connected 和旧 IP -> 标记 reconnecting
 *                  -> 没有 AP 客户端：立即、2 秒、5 秒、每 10 秒重试
 *                  -> 已有 AP 客户端：暂停 STA 扫描
 *
 *    ESP32-S3 只有一套 2.4 GHz 射频。STA 搜索已关闭的路由器时需要切换信道，
 *    会打断 SoftAP 与手机的通信。因此只要 SoftAP 仍有客户端，就取消尚未
 *    开始的 STA 重试；最后一个客户端离开后再恢复扫描。这样无外网时手机仍
 *    可稳定访问 192.168.4.1 所在的本地网络。重连成功后刷新 SoftAP DNS，
 *    外网恢复。断线不会删除或改写 NVS 中保存的 STA/AP 选择。
 *
 * 4. 再次启动
 *
 *      分别读取并校验 STA、AP NVS 配置 -> 恢复 STA/AP/APSTA 模式
 *                       |
 *                       v
 *      自动开启已保存的 AP，并自动连接已保存的 STA
 *
 *    用户不需要再次进入 WiFi 页面点击。若仅保存过其中一项，则只恢复该项；
 *    擦除 NVS 后则重新回到第 1 种状态。
 */
#include "wifi_manager.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "flash_nvs.h"

#define WIFI_MANAGER_PASSWORD_MAX_LENGTH 64U
#define WIFI_MANAGER_AP_CHANNEL 1U
#define WIFI_MANAGER_AP_MAX_CLIENTS 4U
#define WIFI_MANAGER_COMMAND_QUEUE_LENGTH 1U
#define WIFI_MANAGER_COMMAND_TASK_STACK_SIZE 4096U
#define WIFI_MANAGER_COMMAND_TASK_PRIORITY 5U
/* 与 ESP-IDF softap_sta 官方示例一致：让 DHCP 向客户端提供 DNS 地址。 */
#define WIFI_MANAGER_DHCPS_OFFER_DNS 0x02U
/*
 * STA 断线后的重连间隔。第一次立即重试，随后逐步降低全信道占用和 CPU
 * 调度压力；到达 10 秒后保持 10 秒，不限制总重试次数。
 */
static const uint32_t s_sta_reconnect_delays_ms[] = {0U, 2000U, 5000U, 10000U};

typedef struct {
    const char *ssid;
    const char *password;
} wifi_profile_t;

typedef enum {
    WIFI_COMMAND_SELECT_PROFILE,
    WIFI_COMMAND_CLEAR_PROFILES,
} wifi_command_type_t;

typedef struct {
    wifi_command_type_t type;
    size_t profile_index;
} wifi_command_t;

static const wifi_profile_t s_sta_profiles[WIFI_MANAGER_PROFILE_COUNT] = {
    {.ssid = "tom123", .password = "123456789"},
    {.ssid = "tom1234", .password = "123456789"},
    {.ssid = "tom12345", .password = "123456789"},
};

static const wifi_profile_t s_ap_profiles[WIFI_MANAGER_PROFILE_COUNT] = {
    {.ssid = "ap_tom123", .password = "123456789"},
    {.ssid = "ap_tom1234", .password = "123456789"},
    {.ssid = "ap_tom12345", .password = "123456789"},
};

static const char *TAG = "WIFI_MANAGER";
/*
 * 队列长度为 1：任务忙时再次点击，只保留尚未执行的最新选择或删除命令。
 * 队列控制块、任务控制块和任务栈均静态创建，不占用运行期堆。
 */
static QueueHandle_t s_command_queue;
static StaticQueue_t s_command_queue_control_block;
static uint8_t s_command_queue_storage[
    WIFI_MANAGER_COMMAND_QUEUE_LENGTH * sizeof(wifi_command_t)];
static TaskHandle_t s_command_task;
static StaticTask_t s_command_task_control_block;
/* NVS 写 Flash 时会关闭 Cache，因此该任务栈必须保留在内部 RAM。 */
static StackType_t
    s_command_task_stack[WIFI_MANAGER_COMMAND_TASK_STACK_SIZE];
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static wifi_manager_status_t s_status;
static char s_sta_password[WIFI_MANAGER_PASSWORD_MAX_LENGTH];
static char s_ap_password[WIFI_MANAGER_PASSWORD_MAX_LENGTH];
static bool s_initialized;
static bool s_wifi_started;
static bool s_sta_reconfiguring;
static esp_timer_handle_t s_sta_reconnect_timer;
/* 下次断线应使用 s_sta_reconnect_delays_ms[] 中的哪个间隔。 */
static size_t s_sta_reconnect_delay_index;

static esp_err_t apply_sta_profile(size_t profile_index);
static esp_err_t apply_ap_profile(size_t profile_index);
static esp_err_t clear_saved_profiles(void);
static void wifi_command_task(void *argument);

/** 把常见的 WiFi 断开 reason 转成便于串口分析的短文本。 */
static const char *wifi_disconnect_reason_text(uint16_t reason)
{
    switch (reason) {
        case WIFI_REASON_UNSPECIFIED:
            return "unspecified";
        case WIFI_REASON_AUTH_LEAVE:
            return "authentication leave";
        case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:
            return "inactivity";
        case WIFI_REASON_ASSOC_LEAVE:
            return "association leave";
        case WIFI_REASON_MISSING_ACKS:
            return "missing ACKs";
        case WIFI_REASON_STA_LEAVING:
            return "station leaving";
        case WIFI_REASON_TIMEOUT:
            return "timeout";
        case WIFI_REASON_PEER_INITIATED:
            return "client initiated";
        case WIFI_REASON_AP_INITIATED:
            return "SoftAP initiated";
        case WIFI_REASON_BEACON_TIMEOUT:
            return "beacon timeout";
        case WIFI_REASON_NO_AP_FOUND:
            return "upstream AP not found";
        case WIFI_REASON_AUTH_FAIL:
            return "authentication failed";
        case WIFI_REASON_ASSOC_FAIL:
            return "association failed";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "handshake timeout";
        case WIFI_REASON_SA_QUERY_TIMEOUT:
            return "SA query timeout";
        default:
            return "other";
    }
}

/** 根据 SSID 查找代码中固定的 WiFi 配置，找不到时返回 NULL。 */
static const wifi_profile_t *find_profile_by_ssid(
    const wifi_profile_t *profiles,
    const char *ssid)
{
    for (size_t index = 0; index < WIFI_MANAGER_PROFILE_COUNT; ++index) {
        if (strcmp(profiles[index].ssid, ssid) == 0) {
            return &profiles[index];
        }
    }
    return NULL;
}

/**
 * @brief 从 Flash 恢复上次选择的 STA 和 SoftAP 配置。
 *
 * STA 与 AP 独立读取。Flash 中没有记录，或者保存的 SSID 不在当前代码的
 * 固定配置表中，都记为 ESP_ERR_NOT_FOUND；函数仍会继续检查另一项，使有效
 * 的单项配置可以恢复。找到匹配 SSID 后使用代码中对应的固定密码，最后把
 * 有效 SSID 发布到状态快照。
 *
 * @return ESP_OK 两项均匹配；ESP_ERR_NOT_FOUND 至少一项没有有效保存配置；
 *         其他值表示读取 Flash 失败。
 */
static esp_err_t load_saved_wifi_profiles(void)
{
    char sta_ssid[WIFI_MANAGER_SSID_MAX_LENGTH + 1U] = {0};
    char ap_ssid[WIFI_MANAGER_SSID_MAX_LENGTH + 1U] = {0};
    bool profile_not_found = false;

    esp_err_t result = flash_nvs_load_wifi_credentials(
        FLASH_NVS_WIFI_TARGET_STA,
        sta_ssid,
        sizeof(sta_ssid),
        s_sta_password,
        sizeof(s_sta_password));
    if (result == ESP_ERR_NOT_FOUND) {
        profile_not_found = true;
    } else if (result != ESP_OK) {
        return result;
    }

    const wifi_profile_t *saved_sta_profile = NULL;
    if (result == ESP_OK) {
        saved_sta_profile = find_profile_by_ssid(s_sta_profiles, sta_ssid);
        if (saved_sta_profile == NULL) {
            profile_not_found = true;
        }
    }
    if (saved_sta_profile != NULL) {
        s_status.sta_configured = true;
        snprintf(s_sta_password,
                 sizeof(s_sta_password),
                 "%s",
                 saved_sta_profile->password);
    } else {
        /* 没有记录或 SSID 不在代码列表中，等待用户点击 STA 按钮重新写入。 */
        sta_ssid[0] = '\0';
        s_sta_password[0] = '\0';
        ESP_LOGI(TAG, "No saved STA selection; waiting for UI");
    }

    result = flash_nvs_load_wifi_credentials(FLASH_NVS_WIFI_TARGET_AP,
                                             ap_ssid,
                                             sizeof(ap_ssid),
                                             s_ap_password,
                                             sizeof(s_ap_password));
    if (result == ESP_ERR_NOT_FOUND) {
        profile_not_found = true;
    } else if (result != ESP_OK) {
        return result;
    }

    const wifi_profile_t *saved_ap_profile = NULL;
    if (result == ESP_OK) {
        saved_ap_profile = find_profile_by_ssid(s_ap_profiles, ap_ssid);
        if (saved_ap_profile == NULL) {
            profile_not_found = true;
        }
    }
    if (saved_ap_profile != NULL) {
        s_status.ap_configured = true;
        snprintf(s_ap_password,
                 sizeof(s_ap_password),
                 "%s",
                 saved_ap_profile->password);
    } else {
        /* 没有记录或 SSID 不在代码列表中，等待用户点击 AP 按钮重新写入。 */
        ap_ssid[0] = '\0';
        s_ap_password[0] = '\0';
        ESP_LOGI(TAG, "No saved SoftAP selection; waiting for UI");
    }

    /* 未配置的一侧保持空字符串，UI 可直接通过状态快照判断并显示。 */
    snprintf(s_status.sta_ssid, sizeof(s_status.sta_ssid), "%s", sta_ssid);
    snprintf(s_status.ap_ssid, sizeof(s_status.ap_ssid), "%s", ap_ssid);
    return profile_not_found ? ESP_ERR_NOT_FOUND : ESP_OK;
}

/**
 * @brief 根据当前已选择的 STA/AP 配置计算 WiFi 驱动工作模式。
 *
 * - STA 和 AP 都已选择：WIFI_MODE_APSTA；
 * - 只选择 STA：WIFI_MODE_STA；
 * - 只选择 AP：WIFI_MODE_AP。
 *
 * 本函数只在至少有一项配置或正在应用新配置时调用，因此不处理“两项都未
 * 选择”的 idle 情况；idle 状态由 start_saved_configuration() 提前返回。
 *
 * @return 应传给 esp_wifi_set_mode() 的模式。
 */
static wifi_mode_t selected_wifi_mode(void)
{
    if (s_status.sta_configured && s_status.ap_configured) {
        return WIFI_MODE_APSTA;
    }
    return s_status.sta_configured ? WIFI_MODE_STA : WIFI_MODE_AP;
}

/**
 * @brief 根据固定账号构造 ESP-IDF STA 配置。
 *
 * 三个候选 SSID 都是固定且互不相同的，因此使用快速扫描：找到第一个满足
 * SSID、认证方式和信号门限的热点后立即连接，不再为挑选同名热点继续扫描
 * 其他信道。这样可缩短 STA 重连占用射频的时间，减轻对 SoftAP 和 BLE 的
 * 影响。最低认证方式限定为 WPA2-PSK，并声明支持但不强制使用 PMF。
 *
 * @param[in] ssid 要连接的上级 WiFi 名称。
 * @param[in] password 上级 WiFi 密码。
 * @return 可直接传给 esp_wifi_set_config(WIFI_IF_STA, ...) 的配置副本。
 */
static wifi_config_t make_sta_config(const char *ssid, const char *password)
{
    wifi_config_t config = {0};
    snprintf((char *)config.sta.ssid,
             sizeof(config.sta.ssid),
             "%s",
             ssid);
    snprintf((char *)config.sta.password,
             sizeof(config.sta.password),
             "%s",
             password);
    config.sta.scan_method = WIFI_FAST_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    return config;
}

/**
 * @brief 根据固定账号构造 ESP-IDF SoftAP 配置。
 *
 * 单独运行 AP 时先使用 WIFI_MANAGER_AP_CHANNEL；进入 APSTA 后，由于 ESP32-S3
 * 只有一套 2.4 GHz 射频，SoftAP 会自动跟随 STA 连接的信道。热点最多允许
 * WIFI_MANAGER_AP_MAX_CLIENTS 个客户端，并使用 WPA2-PSK。
 *
 * @param[in] ssid ESP32-S3 对外广播的热点名称。
 * @param[in] password SoftAP 密码。
 * @return 可直接传给 esp_wifi_set_config(WIFI_IF_AP, ...) 的配置副本。
 */
static wifi_config_t make_ap_config(const char *ssid, const char *password)
{
    wifi_config_t config = {0};
    snprintf((char *)config.ap.ssid,
             sizeof(config.ap.ssid),
             "%s",
             ssid);
    snprintf((char *)config.ap.password,
             sizeof(config.ap.password),
             "%s",
             password);
    config.ap.ssid_len = strlen(ssid);
    config.ap.channel = WIFI_MANAGER_AP_CHANNEL;
    config.ap.max_connection = WIFI_MANAGER_AP_MAX_CLIENTS;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.pmf_cfg.required = false;
    return config;
}

/**
 * @brief 把 STA 从上级网络获得的 DNS 交给 SoftAP DHCP 服务器。
 *
 * NAPT 负责 IP/端口转换，DNS 地址则必须由 DHCP 另行告诉客户端。缺少这一步
 * 时客户端可能能访问公网 IP，却无法通过域名访问网站。
 *
 * 调用时机有两个：STA 获得 IP 时；或者 STA 已连接后用户才开启 SoftAP 时。
 * 更新过程先暂停 SoftAP DHCP 服务，写入“提供 DNS”选项和 DNS 地址，再重新
 * 启动 DHCP。任一步骤失败只记录日志；事件回调不能把错误返回给调用者。
 */
static void update_softap_dns_from_sta(void)
{
    if (s_ap_netif == NULL || s_sta_netif == NULL) {
        return;
    }

    esp_netif_dns_info_t dns = {0};
    esp_err_t result = esp_netif_get_dns_info(s_sta_netif,
                                               ESP_NETIF_DNS_MAIN,
                                               &dns);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Cannot read STA DNS: %s", esp_err_to_name(result));
        return;
    }

    const uint8_t offer_dns = WIFI_MANAGER_DHCPS_OFFER_DNS;
    const esp_err_t stop_result = esp_netif_dhcps_stop(s_ap_netif);
    if (stop_result != ESP_OK) {
        ESP_LOGD(TAG,
                 "SoftAP DHCP stop before DNS update: %s",
                 esp_err_to_name(stop_result));
    }

    result = esp_netif_dhcps_option(s_ap_netif,
                                    ESP_NETIF_OP_SET,
                                    ESP_NETIF_DOMAIN_NAME_SERVER,
                                    (void *)&offer_dns,
                                    sizeof(offer_dns));
    if (result == ESP_OK) {
        result = esp_netif_set_dns_info(s_ap_netif,
                                        ESP_NETIF_DNS_MAIN,
                                        &dns);
    }

    const esp_err_t start_result = esp_netif_dhcps_start(s_ap_netif);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Cannot update SoftAP DNS: %s", esp_err_to_name(result));
    } else if (start_result != ESP_OK) {
        ESP_LOGW(TAG,
                 "Cannot restart SoftAP DHCP: %s",
                 esp_err_to_name(start_result));
    }
}

/**
 * @brief 在已经启动的 SoftAP 网卡上开启 IPv4 NAPT。
 *
 * AP 客户端发出的私网数据包从 SoftAP 网卡进入，NAPT 把源地址和端口转换为
 * ESP32-S3 STA 的地址，再通过默认 STA 网卡发往外网。s_status.napt_enabled
 * 用于避免重复调用底层接口；状态读写使用临界区与事件回调同步。
 *
 * ESP-IDF 要求目标 Netif 已经 up，否则 esp_netif_napt_enable() 返回
 * ESP_FAIL。因此本函数只能通过 configure_softap_uplink_if_ready() 在
 * WIFI_EVENT_AP_START 到达后调用，不能紧跟 esp_wifi_start() 调用。
 *
 * @return ESP_OK 已经开启或本次开启成功；其他值为底层接口错误。
 */
static esp_err_t enable_softap_napt(void)
{
    portENTER_CRITICAL(&s_status_lock);
    const bool already_enabled = s_status.napt_enabled;
    portEXIT_CRITICAL(&s_status_lock);
    if (already_enabled) {
        return ESP_OK;
    }

    const esp_err_t result = esp_netif_napt_enable(s_ap_netif);
    if (result == ESP_OK) {
        portENTER_CRITICAL(&s_status_lock);
        s_status.napt_enabled = true;
        portEXIT_CRITICAL(&s_status_lock);
        ESP_LOGI(TAG, "SoftAP NAPT enabled; AP clients use the STA uplink");
    }
    return result;
}

/**
 * @brief 在 STA 和 SoftAP 都就绪后配置 AP 客户端的外网出口。
 *
 * AP 启动和 STA 获取 IP 都是异步事件，先后顺序不固定。本函数由两个事件各
 * 调用一次：只要当时两侧尚未同时就绪就直接返回；后到的事件负责设置 STA
 * 为默认出口、把上级 DNS 写入 SoftAP DHCP，并在已经 up 的 AP Netif 上开启
 * NAPT。这样首次选择配置、重启恢复和 STA 重连共用同一条可靠路径。
 */
static void configure_softap_uplink_if_ready(void)
{
    portENTER_CRITICAL(&s_status_lock);
    const bool ready = s_status.sta_connected && s_status.ap_started;
    portEXIT_CRITICAL(&s_status_lock);
    if (!ready) {
        return;
    }

    const esp_err_t default_result =
        esp_netif_set_default_netif(s_sta_netif);
    if (default_result != ESP_OK) {
        ESP_LOGW(TAG,
                 "Cannot set STA as default interface: %s",
                 esp_err_to_name(default_result));
        return;
    }

    update_softap_dns_from_sta();
    const esp_err_t napt_result = enable_softap_napt();
    if (napt_result != ESP_OK) {
        ESP_LOGW(TAG,
                 "Cannot enable SoftAP NAPT after interfaces are ready: %s",
                 esp_err_to_name(napt_result));
    }
}

/**
 * @brief 延迟结束后发起一次 STA 重连。
 *
 * esp_timer 回调运行在系统定时器任务中。回调只读取很短的共享状态并调用
 * 非阻塞的 esp_wifi_connect()；扫描、认证和 DHCP 都由 WiFi 驱动异步完成。
 * 如果等待期间已经连上、切换了 STA 配置、取消了 STA，或者 SoftAP 已有
 * 客户端，本次回调直接退出。最后一项用于防止定时器到期与手机接入同时发生
 * 时仍启动跨信道扫描。
 */
static void sta_reconnect_timer_callback(void *argument)
{
    (void)argument;

    portENTER_CRITICAL(&s_status_lock);
    const bool should_reconnect = s_status.sta_configured &&
                                  s_status.sta_reconnecting &&
                                  !s_status.sta_connected &&
                                  !s_sta_reconfiguring &&
                                  s_status.ap_client_count == 0U;
    portEXIT_CRITICAL(&s_status_lock);
    if (!should_reconnect) {
        return;
    }

    const esp_err_t result = esp_wifi_connect();
    if (result != ESP_OK && result != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG,
                 "STA reconnect request failed: %s",
                 esp_err_to_name(result));
    }
}

/**
 * @brief 在 SoftAP 没有客户端时安排下一次 STA 重连。
 *
 * 每次 DISCONNECTED 事件推进一级间隔，达到 10 秒后不再增长。连接成功时
 * IP_EVENT_STA_GOT_IP 会把级别清零，因此以后再次掉线仍先立即重连。延迟期间
 * SoftAP 保持工作，BLE 也能获得更多射频时间片。
 *
 * 只要 SoftAP 有客户端，就不启动 STA 扫描，也不推进退避级别。ESP32-S3 的
 * STA 和 SoftAP 共用一套射频，暂停跨信道扫描可以保证无外网时的本地 AP
 * 连接优先。最后一个 AP 客户端离开时，事件处理函数会再次调用本函数。
 */
static void schedule_sta_reconnect(void)
{
    const size_t delay_count = sizeof(s_sta_reconnect_delays_ms) /
                               sizeof(s_sta_reconnect_delays_ms[0]);

    portENTER_CRITICAL(&s_status_lock);
    const bool reconnect_pending = s_status.sta_configured &&
                                   !s_status.sta_connected &&
                                   !s_sta_reconfiguring;
    const uint8_t ap_client_count = s_status.ap_client_count;
    const bool should_reconnect = reconnect_pending &&
                                  ap_client_count == 0U;
    size_t delay_index = s_sta_reconnect_delay_index;
    if (delay_index >= delay_count) {
        delay_index = delay_count - 1U;
    }
    const uint32_t delay_ms = s_sta_reconnect_delays_ms[delay_index];
    if (should_reconnect &&
        s_sta_reconnect_delay_index < delay_count - 1U) {
        ++s_sta_reconnect_delay_index;
    }
    portEXIT_CRITICAL(&s_status_lock);

    if (reconnect_pending && ap_client_count > 0U) {
        (void)esp_timer_stop(s_sta_reconnect_timer);
        ESP_LOGI(TAG,
                 "STA reconnect paused: %u SoftAP client(s) connected",
                 (unsigned)ap_client_count);
        return;
    }
    if (!should_reconnect) {
        return;
    }
    if (delay_ms == 0U) {
        ESP_LOGI(TAG, "STA reconnecting immediately");
        sta_reconnect_timer_callback(NULL);
        return;
    }

    /* 单次定时器理论上已经停止；先 stop 可兼容重复到达的断线事件。 */
    (void)esp_timer_stop(s_sta_reconnect_timer);
    const esp_err_t result = esp_timer_start_once(
        s_sta_reconnect_timer,
        (uint64_t)delay_ms * 1000ULL);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "STA reconnect scheduled in %" PRIu32 " ms", delay_ms);
    } else {
        ESP_LOGW(TAG,
                 "Cannot schedule STA reconnect: %s",
                 esp_err_to_name(result));
    }
}

/**
 * @brief 统一处理 WiFi 驱动事件和 STA 获取 IP 事件。
 *
 * 回调运行在 ESP-IDF 默认事件循环任务中，不创建页面，也不访问 LVGL。它只做
 * 状态更新、必要的网络配置和短时间日志操作：
 *
 * - WIFI_EVENT_STA_START：记录 STA 接口已启动；连接由初始化/选择函数发起；
 * - WIFI_EVENT_STA_DISCONNECTED：清除旧 IP，并按退避间隔安排重连；
 * - IP_EVENT_STA_GOT_IP：标记连接成功，并在 AP 已启动时配置 DNS/NAPT；
 * - WIFI_EVENT_AP_START：标记 AP 已启动，并在 STA 已联网时配置 DNS/NAPT；
 * - WIFI_EVENT_AP_STOP：清理 AP 运行状态；
 * - AP_STACONNECTED/AP_STADISCONNECTED：维护当前 AP 客户端数量。
 *
 * STA 断线不会停止 SoftAP，也不会删除 NVS。s_status 同时被 UI 任务读取，因此
 * 每次修改都由 s_status_lock 保护；耗时的 WiFi/Netif API 在退出临界区后调用。
 *
 * @param[in] argument 注册时的用户参数，本模块固定传 NULL。
 * @param[in] event_base WIFI_EVENT 或 IP_EVENT。
 * @param[in] event_id event_base 下的具体事件编号。
 * @param[in] event_data 事件数据；其结构类型由 event_base/event_id 决定。
 */
static void wifi_event_handler(void *argument,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)argument;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA interface started");
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = event_data;
        portENTER_CRITICAL(&s_status_lock);
        s_status.sta_connected = false;
        s_status.sta_ip[0] = '\0';
        const bool reconnect = s_status.sta_configured &&
                               !s_sta_reconfiguring;
        s_status.sta_reconnecting = reconnect;
        portEXIT_CRITICAL(&s_status_lock);

        ESP_LOGW(TAG,
                 "STA disconnected, reason=%u (%s)%s",
                 (unsigned)event->reason,
                 wifi_disconnect_reason_text(event->reason),
                 reconnect ? "; retry pending" : "");
        if (reconnect) {
            schedule_sta_reconnect();
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        char ip_text[WIFI_MANAGER_IPV4_TEXT_LENGTH] = {0};
        char sta_ssid[WIFI_MANAGER_SSID_MAX_LENGTH + 1U];
        snprintf(ip_text,
                 sizeof(ip_text),
                 IPSTR,
                 IP2STR(&event->ip_info.ip));
        portENTER_CRITICAL(&s_status_lock);
        s_status.sta_connected = true;
        s_status.sta_reconnecting = false;
        s_sta_reconnect_delay_index = 0U;
        memcpy(s_status.sta_ip, ip_text, sizeof(s_status.sta_ip));
        memcpy(sta_ssid, s_status.sta_ssid, sizeof(sta_ssid));
        const bool ap_configured = s_status.ap_configured;
        portEXIT_CRITICAL(&s_status_lock);

        /* 防止恰好在获得 IP 前已经排入的延迟重连再次触发。 */
        (void)esp_timer_stop(s_sta_reconnect_timer);

        if (ap_configured) {
            configure_softap_uplink_if_ready();
        } else {
            (void)esp_netif_set_default_netif(s_sta_netif);
        }
        ESP_LOGI(TAG,
                 "STA connected to %s, IP=" IPSTR,
                 sta_ssid,
                 IP2STR(&event->ip_info.ip));
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        esp_netif_ip_info_t ip_info = {0};
        (void)esp_netif_get_ip_info(s_ap_netif, &ip_info);
        char ip_text[WIFI_MANAGER_IPV4_TEXT_LENGTH] = {0};
        char ap_ssid[WIFI_MANAGER_SSID_MAX_LENGTH + 1U];
        snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&ip_info.ip));
        portENTER_CRITICAL(&s_status_lock);
        s_status.ap_started = true;
        memcpy(s_status.ap_ip, ip_text, sizeof(s_status.ap_ip));
        memcpy(ap_ssid, s_status.ap_ssid, sizeof(ap_ssid));
        portEXIT_CRITICAL(&s_status_lock);
        ESP_LOGI(TAG, "SoftAP %s started", ap_ssid);
        configure_softap_uplink_if_ready();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        portENTER_CRITICAL(&s_status_lock);
        s_status.ap_started = false;
        s_status.napt_enabled = false;
        s_status.ap_client_count = 0U;
        s_status.ap_ip[0] = '\0';
        portEXIT_CRITICAL(&s_status_lock);
        return;
    }

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *event = event_data;
        wifi_sta_list_t station_list = {0};
        const esp_err_t list_result =
            esp_wifi_ap_get_sta_list(&station_list);

        portENTER_CRITICAL(&s_status_lock);
        if (list_result == ESP_OK) {
            s_status.ap_client_count = (uint8_t)station_list.num;
        } else if (s_status.ap_client_count < UINT8_MAX) {
            /* 读取驱动列表失败时才使用事件计数作为后备。 */
            ++s_status.ap_client_count;
        }
        const uint8_t client_count = s_status.ap_client_count;
        const bool sta_reconnect_paused = s_status.sta_configured &&
                                          s_status.sta_reconnecting &&
                                          !s_status.sta_connected;
        portEXIT_CRITICAL(&s_status_lock);

        /* 手机已经接入本地 AP 后，不再让待执行的 STA 扫描抢占射频。 */
        if (sta_reconnect_paused) {
            (void)esp_timer_stop(s_sta_reconnect_timer);
        }
        if (list_result != ESP_OK) {
            ESP_LOGW(TAG,
                     "Cannot read SoftAP client list after connect: %s",
                     esp_err_to_name(list_result));
        }
        ESP_LOGI(TAG,
                 "SoftAP client connected: MAC=" MACSTR
                 ", AID=%u, clients=%u%s",
                 MAC2STR(event->mac),
                 (unsigned)event->aid,
                 (unsigned)client_count,
                 sta_reconnect_paused ? "; STA scans paused" : "");
        return;
    }

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *event = event_data;
        wifi_sta_list_t station_list = {0};
        const esp_err_t list_result =
            esp_wifi_ap_get_sta_list(&station_list);

        portENTER_CRITICAL(&s_status_lock);
        if (list_result == ESP_OK) {
            s_status.ap_client_count = (uint8_t)station_list.num;
        } else if (s_status.ap_client_count > 0U) {
            /* 读取驱动列表失败时才使用事件计数作为后备。 */
            --s_status.ap_client_count;
        }
        const uint8_t client_count = s_status.ap_client_count;
        const bool resume_sta_reconnect = client_count == 0U &&
                                          s_status.sta_configured &&
                                          s_status.sta_reconnecting &&
                                          !s_status.sta_connected &&
                                          !s_sta_reconfiguring;
        portEXIT_CRITICAL(&s_status_lock);
        if (list_result != ESP_OK) {
            ESP_LOGW(TAG,
                     "Cannot read SoftAP client list after disconnect: %s",
                     esp_err_to_name(list_result));
        }
        ESP_LOGI(TAG,
                 "SoftAP client disconnected: MAC=" MACSTR
                 ", AID=%u, reason=%u (%s), clients=%u",
                 MAC2STR(event->mac),
                 (unsigned)event->aid,
                 (unsigned)event->reason,
                 wifi_disconnect_reason_text(event->reason),
                 (unsigned)client_count);
        if (resume_sta_reconnect) {
            ESP_LOGI(TAG,
                     "Last SoftAP client left; STA reconnect may resume");
            schedule_sta_reconnect();
        }
    }
}

/**
 * @brief 启动从 NVS 恢复的 STA、AP 或 APSTA 配置。
 *
 * 两项都没有保存时保持 idle，不启动射频；保存一项时启动 STA 或 AP；两项
 * 都保存时启动 APSTA 并连接 STA。NAPT 要等待 AP Netif 已经 up 且 STA 已经
 * 获得 IP，再由事件处理函数开启。
 *
 * 执行顺序：
 *
 *   计算模式 -> 写入已保存的 STA/AP 驱动配置 -> esp_wifi_start()
 *            -> STA 已配置则发起连接
 *            -> AP_START 与 STA_GOT_IP 都满足后配置 DNS/NAPT
 *
 * 本函数只在 wifi_manager_init() 完成 Netif、事件循环和 WiFi 驱动初始化后
 * 调用。配置恢复失败时立即返回错误，不继续执行后续步骤。
 *
 * @return ESP_OK 无保存配置或恢复成功；其他值为 WiFi/NAPT 接口错误。
 */
static esp_err_t start_saved_configuration(void)
{
    if (!s_status.sta_configured && !s_status.ap_configured) {
        ESP_LOGI(TAG, "No saved WiFi profile; waiting for UI selection");
        return ESP_OK;
    }

    esp_err_t result = esp_wifi_set_mode(selected_wifi_mode());
    if (result != ESP_OK) {
        return result;
    }

    if (s_status.sta_configured) {
        wifi_config_t sta_config = make_sta_config(s_status.sta_ssid,
                                                   s_sta_password);
        result = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        if (result != ESP_OK) {
            return result;
        }
    }
    if (s_status.ap_configured) {
        wifi_config_t ap_config = make_ap_config(s_status.ap_ssid,
                                                 s_ap_password);
        result = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        if (result != ESP_OK) {
            return result;
        }
    }

    result = esp_wifi_start();
    if (result != ESP_OK) {
        return result;
    }
    s_wifi_started = true;

    if (s_status.sta_configured) {
        portENTER_CRITICAL(&s_status_lock);
        s_status.sta_reconnecting = true;
        portEXIT_CRITICAL(&s_status_lock);
        result = esp_wifi_connect();
        if (result != ESP_OK) {
            return result;
        }
    }
    return ESP_OK;
}

/**
 * @brief 初始化常驻 WiFi 管理服务并恢复上次选择。
 *
 * 必须在 nvs_flash_init() 成功之后调用，并且整个系统只调用一次。函数依次：
 *
 * 1. 从 flash_nvs 读取 STA 与 AP 的 SSID，并与代码中的固定配置表核对；
 * 2. 初始化 TCP/IP 栈和默认事件循环，创建 STA、SoftAP 两个 esp_netif；
 * 3. 注册 WiFi 全部事件以及 STA 获取 IP 事件；
 * 4. 初始化 WiFi 驱动，并把驱动自身存储方式设为 RAM；
 * 5. 创建 STA 退避重连使用的单次定时器；
 * 6. 创建配置队列和使用内部 RAM 栈的配置任务；
 * 7. 发布 ready 状态，然后按 NVS 内容恢复 idle、STA、AP 或 APSTA。
 *
 * 使用 WIFI_STORAGE_RAM 是为了避免 ESP-IDF WiFi 驱动在 Flash 中再维护一份
 * 隐式配置。需要跨重启保存的账号只由 flash_nvs.c 管理。
 *
 * @return ESP_OK 初始化和配置恢复成功；ESP_ERR_INVALID_STATE 表示重复调用；
 *         ESP_ERR_NO_MEM 表示队列、任务或网卡对象创建失败；其他值来自
 *         NVS、Netif、
 *         事件循环、WiFi 驱动或 NAPT。
 */
esp_err_t wifi_manager_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = load_saved_wifi_profiles();
    if (result == ESP_ERR_NOT_FOUND) {
        /* 没有完整的保存配置不是初始化错误，继续启动并等待页面按钮。 */
        ESP_LOGI(TAG, "Saved WiFi selection is incomplete; waiting for UI");
    } else if (result != ESP_OK) {
        return result;
    }

    /*
     * esp_netif_init() 和默认事件循环可能已经被其他 BSP 初始化；这种情况下
     * ESP_ERR_INVALID_STATE 表示资源已存在，可以继续复用，不属于失败。
     */
    result = esp_netif_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }
    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }

    /* 两个默认网卡同时创建，为以后从单模式切换到 APSTA 预留完整对象。 */
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_sta_netif == NULL || s_ap_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* WiFi 状态变化和 STA DHCP 成功事件都交给同一个轻量回调处理。 */
    result = esp_event_handler_instance_register(WIFI_EVENT,
                                                 ESP_EVENT_ANY_ID,
                                                 wifi_event_handler,
                                                 NULL,
                                                 NULL);
    if (result != ESP_OK) {
        return result;
    }
    result = esp_event_handler_instance_register(IP_EVENT,
                                                 IP_EVENT_STA_GOT_IP,
                                                 wifi_event_handler,
                                                 NULL,
                                                 NULL);
    if (result != ESP_OK) {
        return result;
    }

    /*
     * 驱动配置使用 ESP-IDF 默认值，但明确关闭其内部 NVS。账号和密码已经由
     * flash_nvs.c 统一保存，驱动再维护一份会重复占用 Flash 和运行期资源。
     * PHY 校准数据由系统 PHY/NVS 组件管理，不依赖这里的 WiFi 配置存储。
     */
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    init_config.nvs_enable = 0;
    result = esp_wifi_init(&init_config);
    if (result != ESP_OK) {
        return result;
    }
    result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result != ESP_OK) {
        return result;
    }

    /*
     * 只创建一个可重复启动的单次定时器，不为每次掉线分配新对象。回调只发出
     * 异步连接请求，不在定时器任务中等待扫描、认证或 DHCP。
     */
    const esp_timer_create_args_t reconnect_timer_args = {
        .callback = sta_reconnect_timer_callback,
        .name = "wifi_sta_retry",
    };
    result = esp_timer_create(&reconnect_timer_args, &s_sta_reconnect_timer);
    if (result != ESP_OK) {
        return result;
    }
    (void)esp_netif_set_default_netif(s_sta_netif);

    /*
     * UI 只写入一个很小的静态队列。任务栈明确位于内部 RAM，因为后续
     * flash_nvs_save_wifi_credentials() 写 Flash 时会暂时关闭外部 Cache。
     */
    s_command_queue = xQueueCreateStatic(
        WIFI_MANAGER_COMMAND_QUEUE_LENGTH,
        sizeof(wifi_command_t),
        s_command_queue_storage,
        &s_command_queue_control_block);
    if (s_command_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_command_task = xTaskCreateStatic(wifi_command_task,
                                       "wifi_command",
                                       WIFI_MANAGER_COMMAND_TASK_STACK_SIZE,
                                       NULL,
                                       WIFI_MANAGER_COMMAND_TASK_PRIORITY,
                                       s_command_task_stack,
                                       &s_command_task_control_block);
    if (s_command_task == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 从此刻起，页面可以读取状态并提交配置选择。 */
    s_initialized = true;
    portENTER_CRITICAL(&s_status_lock);
    s_status.ready = true;
    portEXIT_CRITICAL(&s_status_lock);

    /* 有保存配置就立即恢复；没有配置时保持 idle 并等待页面第一次选择。 */
    result = start_saved_configuration();
    if (result == ESP_OK) {
        ESP_LOGI(TAG,
                 "Ready: STA=%s, SoftAP=%s, mode=%s",
                 s_status.sta_configured ? s_status.sta_ssid : "not selected",
                 s_status.ap_configured ? s_status.ap_ssid : "not selected",
                 s_status.sta_configured && s_status.ap_configured
                     ? "APSTA"
                     : (s_status.sta_configured ? "STA" :
                        (s_status.ap_configured ? "AP" : "idle")));
    }
    return result;
}

/**
 * @brief 在 WiFi 配置任务中保存并连接指定序号的 STA 固定配置。
 *
 * 函数只由 wifi_command_task() 调用，执行流程如下：
 *
 *   校验序号 -> 保存到 NVS -> 更新状态快照
 *             -> 切换 STA/APSTA 模式 -> 写入 STA 驱动配置
 *             -> 必要时启动 WiFi -> 断开旧 STA -> 连接新 STA
 *
 * s_sta_reconfiguring 在主动切换期间为 true，防止旧连接产生的 DISCONNECTED
 * 事件为旧配置安排重连；同时停止已有退避定时器。SoftAP 若已运行则始终
 * 保留，切换 STA 时只会暂时中断 AP 客户端的外网出口。
 *
 * 配置先写入 NVS，再应用到驱动。因此即使后续驱动操作偶发失败，下次启动仍
 * 会按用户本次选择重新尝试。
 *
 * @param[in] profile_index STA 固定配置序号，范围为 0 到
 *                          WIFI_MANAGER_PROFILE_COUNT - 1。
 * @return ESP_OK 保存并发起连接成功；ESP_ERR_INVALID_ARG 序号无效；
 *         ESP_ERR_INVALID_STATE 管理器尚未初始化；其他值来自 NVS/WiFi 驱动。
 */
static esp_err_t apply_sta_profile(size_t profile_index)
{
    if (profile_index >= WIFI_MANAGER_PROFILE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    const wifi_profile_t *profile = &s_sta_profiles[profile_index];
    /*
     * 先持久化用户选择，再切换当前连接。运行时若后续配置失败，下次启动仍会
     * 使用这次选择重新尝试；已经开启的 SoftAP 在 STA 切换期间保持运行。
     */
    esp_err_t result = flash_nvs_save_wifi_credentials(
        FLASH_NVS_WIFI_TARGET_STA,
        profile->ssid,
        profile->password);
    if (result != ESP_OK) {
        return result;
    }

    snprintf(s_sta_password, sizeof(s_sta_password), "%s", profile->password);
    portENTER_CRITICAL(&s_status_lock);
    s_sta_reconfiguring = true;
    s_status.sta_configured = true;
    s_status.sta_connected = false;
    s_status.sta_reconnecting = true;
    s_sta_reconnect_delay_index = 0U;
    s_status.sta_ip[0] = '\0';
    memcpy(s_status.sta_ssid,
           profile->ssid,
           strlen(profile->ssid) + 1U);
    portEXIT_CRITICAL(&s_status_lock);

    /* 用户主动换网时取消旧配置留下的延迟重连，稍后直接连接新配置。 */
    (void)esp_timer_stop(s_sta_reconnect_timer);

    result = esp_wifi_set_mode(selected_wifi_mode());
    if (result == ESP_OK) {
        wifi_config_t config = make_sta_config(profile->ssid,
                                               profile->password);
        result = esp_wifi_set_config(WIFI_IF_STA, &config);
    }
    if (result == ESP_OK && !s_wifi_started) {
        result = esp_wifi_start();
        if (result == ESP_OK) {
            s_wifi_started = true;
        }
    }
    if (result == ESP_OK && s_wifi_started) {
        /* 已连旧网络时先断开；断线事件看到 reconfiguring 不会抢先重连。 */
        (void)esp_wifi_disconnect();
    }

    portENTER_CRITICAL(&s_status_lock);
    s_sta_reconfiguring = false;
    portEXIT_CRITICAL(&s_status_lock);

    if (result == ESP_OK) {
        result = esp_wifi_connect();
        if (result == ESP_ERR_WIFI_CONN) {
            /* 断线事件可能已经用新配置发起连接，此时无需再发第二次。 */
            result = ESP_OK;
        }
    }
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "STA profile selected: %s", profile->ssid);
    }
    return result;
}

/**
 * @brief 在 WiFi 配置任务中保存并启用指定序号的 SoftAP 固定配置。
 *
 * 函数只由 wifi_command_task() 调用，执行流程如下：
 *
 *   校验序号 -> 保存到 NVS -> 更新状态快照
 *             -> 切换 AP/APSTA 模式 -> 写入 AP 驱动配置
 *             -> 必要时启动 WiFi
 *             -> AP_START 与 STA_GOT_IP 都满足后异步配置 DNS/NAPT
 *
 * STA 已配置时模式为 APSTA，原 STA 连接继续工作；STA 尚未选择时只开启 AP，
 * 客户端可以连接本地热点，但要等以后选择并连上 STA 才能访问外网。
 *
 * @param[in] profile_index SoftAP 固定配置序号，范围为 0 到
 *                          WIFI_MANAGER_PROFILE_COUNT - 1。
 * @return ESP_OK 保存并启动成功；ESP_ERR_INVALID_ARG 序号无效；
 *         ESP_ERR_INVALID_STATE 管理器尚未初始化；其他值来自 NVS、WiFi 或
 *         NAPT 接口。
 */
static esp_err_t apply_ap_profile(size_t profile_index)
{
    if (profile_index >= WIFI_MANAGER_PROFILE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    const wifi_profile_t *profile = &s_ap_profiles[profile_index];
    /*
     * AP 选择同样先写入 NVS。若 STA 已配置，模式变为 APSTA；若 STA 尚未
     * 选择，则先独立开启 AP，用户以后选择 STA 时再无缝切换到 APSTA。
     */
    esp_err_t result = flash_nvs_save_wifi_credentials(
        FLASH_NVS_WIFI_TARGET_AP,
        profile->ssid,
        profile->password);
    if (result != ESP_OK) {
        return result;
    }

    snprintf(s_ap_password, sizeof(s_ap_password), "%s", profile->password);
    portENTER_CRITICAL(&s_status_lock);
    s_status.ap_configured = true;
    memcpy(s_status.ap_ssid,
           profile->ssid,
           strlen(profile->ssid) + 1U);
    portEXIT_CRITICAL(&s_status_lock);

    result = esp_wifi_set_mode(selected_wifi_mode());
    if (result == ESP_OK) {
        wifi_config_t config = make_ap_config(profile->ssid,
                                              profile->password);
        result = esp_wifi_set_config(WIFI_IF_AP, &config);
    }
    if (result == ESP_OK && !s_wifi_started) {
        result = esp_wifi_start();
        if (result == ESP_OK) {
            s_wifi_started = true;
        }
    }
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "SoftAP profile selected: %s", profile->ssid);
    }
    return result;
}

/** 删除两组 NVS 凭据、停止当前 WiFi，并把运行状态恢复为未选择。 */
static esp_err_t clear_saved_profiles(void)
{
    esp_err_t result = flash_nvs_clear_wifi_credentials();
    if (result != ESP_OK) {
        return result;
    }

    /* 先禁止后续重连，再停止可能仍在运行的 STA 和 SoftAP。 */
    (void)esp_timer_stop(s_sta_reconnect_timer);
    portENTER_CRITICAL(&s_status_lock);
    s_sta_reconfiguring = true;
    const bool disable_napt = s_status.napt_enabled;
    s_status.sta_configured = false;
    s_status.sta_connected = false;
    s_status.sta_reconnecting = false;
    s_status.sta_ssid[0] = '\0';
    s_status.sta_ip[0] = '\0';
    s_status.ap_configured = false;
    s_status.ap_started = false;
    s_status.napt_enabled = false;
    s_status.ap_client_count = 0U;
    s_status.ap_ssid[0] = '\0';
    s_status.ap_ip[0] = '\0';
    s_sta_reconnect_delay_index = 0U;
    portEXIT_CRITICAL(&s_status_lock);

    s_sta_password[0] = '\0';
    s_ap_password[0] = '\0';
    if (disable_napt) {
        const esp_err_t napt_result = esp_netif_napt_disable(s_ap_netif);
        if (napt_result != ESP_OK) {
            ESP_LOGW(TAG,
                     "Cannot disable SoftAP NAPT while clearing: %s",
                     esp_err_to_name(napt_result));
        }
    }

    if (s_wifi_started) {
        result = esp_wifi_stop();
        if (result == ESP_OK) {
            s_wifi_started = false;
        }
    }

    portENTER_CRITICAL(&s_status_lock);
    s_sta_reconfiguring = false;
    portEXIT_CRITICAL(&s_status_lock);
    return result;
}

/**
 * @brief 串行执行 UI 提交的选择或删除 WiFi 配置命令。
 *
 * 该任务没有命令时永久阻塞，不轮询、不占用 CPU。选择命令依次保存并应用
 * STA 与 SoftAP；删除命令擦除两组 NVS 凭据并停止 WiFi。所有 NVS 写入都
 * 发生在本任务的内部 RAM 栈上，Flash Cache 关闭时不会访问 PSRAM 栈。
 */
static void wifi_command_task(void *argument)
{
    (void)argument;

    for (;;) {
        wifi_command_t command;
        if (xQueueReceive(s_command_queue,
                          &command,
                          portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (command.type) {
            case WIFI_COMMAND_SELECT_PROFILE: {
                esp_err_t result = apply_sta_profile(command.profile_index);
                if (result == ESP_OK) {
                    result = apply_ap_profile(command.profile_index);
                }
                if (result == ESP_OK) {
                    ESP_LOGI(TAG,
                             "WiFi profile %u applied",
                             (unsigned)(command.profile_index + 1U));
                } else {
                    ESP_LOGE(TAG,
                             "Cannot apply WiFi profile %u: %s",
                             (unsigned)(command.profile_index + 1U),
                             esp_err_to_name(result));
                }
                break;
            }
            case WIFI_COMMAND_CLEAR_PROFILES: {
                const esp_err_t result = clear_saved_profiles();
                if (result == ESP_OK) {
                    ESP_LOGI(TAG,
                             "Saved STA/AP credentials cleared; WiFi stopped");
                } else {
                    ESP_LOGE(TAG,
                             "Cannot clear saved WiFi credentials: %s",
                             esp_err_to_name(result));
                }
                break;
            }
            default:
                ESP_LOGE(TAG, "Unknown WiFi command");
                break;
        }
    }
}

/**
 * @brief 将组合 WiFi 配置序号异步提交给内部配置任务。
 *
 * 队列长度为 1，xQueueOverwrite() 不会等待。配置任务忙时再次点击会替换尚未
 * 处理的旧命令，只保留用户最后一次选择；正在执行的命令不会被中途打断。
 */
esp_err_t wifi_manager_select_profile(size_t profile_index)
{
    if (profile_index >= WIFI_MANAGER_PROFILE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_command_queue == NULL ||
        s_command_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const wifi_command_t command = {
        .type = WIFI_COMMAND_SELECT_PROFILE,
        .profile_index = profile_index,
    };
    if (xQueueOverwrite(s_command_queue, &command) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t wifi_manager_clear_profiles(void)
{
    if (!s_initialized || s_command_queue == NULL ||
        s_command_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const wifi_command_t command = {
        .type = WIFI_COMMAND_CLEAR_PROFILES,
        .profile_index = 0U,
    };
    if (xQueueOverwrite(s_command_queue, &command) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief 读取一组内置 STA 配置的 SSID，供 UI 构造选择按钮。
 * @param[in] profile_index 固定配置序号。
 * @return 指向只读静态字符串的指针；序号无效时返回 NULL。调用者不得释放。
 */
const char *wifi_manager_get_sta_profile_ssid(size_t profile_index)
{
    return profile_index < WIFI_MANAGER_PROFILE_COUNT
               ? s_sta_profiles[profile_index].ssid
               : NULL;
}

/**
 * @brief 读取一组内置 STA 配置的密码，供 UI 显示当前选择信息。
 * @param[in] profile_index 固定配置序号。
 * @return 指向只读静态字符串的指针；序号无效时返回 NULL。调用者不得释放。
 */
const char *wifi_manager_get_sta_profile_password(size_t profile_index)
{
    return profile_index < WIFI_MANAGER_PROFILE_COUNT
               ? s_sta_profiles[profile_index].password
               : NULL;
}

/**
 * @brief 读取一组内置 SoftAP 配置的 SSID，供 UI 构造选择按钮。
 * @param[in] profile_index 固定配置序号。
 * @return 指向只读静态字符串的指针；序号无效时返回 NULL。调用者不得释放。
 */
const char *wifi_manager_get_ap_profile_ssid(size_t profile_index)
{
    return profile_index < WIFI_MANAGER_PROFILE_COUNT
               ? s_ap_profiles[profile_index].ssid
               : NULL;
}

/**
 * @brief 读取一组内置 SoftAP 配置的密码，供 UI 显示当前选择信息。
 * @param[in] profile_index 固定配置序号。
 * @return 指向只读静态字符串的指针；序号无效时返回 NULL。调用者不得释放。
 */
const char *wifi_manager_get_ap_profile_password(size_t profile_index)
{
    return profile_index < WIFI_MANAGER_PROFILE_COUNT
               ? s_ap_profiles[profile_index].password
               : NULL;
}

/**
 * @brief 原子复制一份 WiFi 当前状态供 UI 或其他任务读取。
 *
 * 事件回调会异步修改 s_status，因此不能把内部结构体地址直接交给调用者。
 * 本函数在短临界区内完成整结构复制，退出后调用者可安全读取自己的副本。
 * configured 字段表示 NVS 中已有选择；connected/started/reconnecting 表示
 * 当前运行状态，两者含义不同。
 *
 * @param[out] status 接收状态快照的结构体。
 * @return ESP_OK 管理器已初始化；ESP_ERR_INVALID_ARG 参数为空；
 *         ESP_ERR_INVALID_STATE 管理器尚未完成初始化。
 */
esp_err_t wifi_manager_get_status(wifi_manager_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_status_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    return s_initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
}
