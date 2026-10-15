/**
 * @file ble_client.c
 * @brief 按需 BLE 浏览扫描及按名称连接、读取 Device Name 的 GATT Client。
 *
 * 模块只有一个 BLE 操作槽位：LVGL 可发起一次纯扫描，CMD 0x02 可发起一次
 * 目标查询。目标查询严格比较 Complete Local Name，命中后停止扫描，使用本次
 * 扫描报告中的地址和地址类型连接，然后依次发现 0x1800、0x2A00 并读取名称。
 */
#include "ble_client.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

/* NimBLE 的扫描 interval/window 使用 0.625 ms 单位：320=200 ms，160=100 ms。 */
#define BLE_CLIENT_INTERVAL_MS 200U
#define BLE_CLIENT_WINDOW_MS 100U
#define BLE_CLIENT_INTERVAL_UNITS 320U
#define BLE_CLIENT_WINDOW_UNITS 160U
/* CMD 目标查询最多扫描 5 秒，找到设备后最多用 2 秒建立连接。 */
#define BLE_CLIENT_QUERY_SCAN_DURATION_MS 5000U
#define BLE_CLIENT_CONNECT_TIMEOUT_MS 2000U
#define BLE_CLIENT_INVALID_CONN_HANDLE UINT16_MAX

/** BLE 硬件操作槽位当前被哪一种业务占用。 */
typedef enum {
    /* 没有扫描或连接操作，LVGL/CMD 都可以发起新请求。 */
    BLE_CLIENT_MODE_IDLE,
    /* LVGL 页面只扫描并收集附近设备，不进行连接。 */
    BLE_CLIENT_MODE_BROWSE,
    /* CMD 0x02 正在按名称扫描、连接和读取 Device Name。 */
    BLE_CLIENT_MODE_QUERY,
} ble_client_mode_t;

/** CMD 目标查询的内部阶段，事件回调必须与当前阶段匹配后才能修改状态。 */
typedef enum {
    BLE_CLIENT_QUERY_NONE,
    BLE_CLIENT_QUERY_SCANNING,
    BLE_CLIENT_QUERY_CONNECTING,
    BLE_CLIENT_QUERY_DISCOVERING_SERVICE,
    BLE_CLIENT_QUERY_DISCOVERING_CHARACTERISTIC,
    BLE_CLIENT_QUERY_READING_NAME,
    BLE_CLIENT_QUERY_ABORTING,
} ble_client_query_phase_t;

/** finish_query() 返回结果前后需要执行的异步 BLE 清理动作。 */
typedef enum {
    /* 当前没有仍在运行的 GAP 操作，可以直接回到 IDLE。 */
    BLE_CLIENT_CLEANUP_NONE,
    /* 查询在扫描阶段终止，需要等待扫描取消完成事件。 */
    BLE_CLIENT_CLEANUP_CANCEL_SCAN,
    /* 已建立连接，需要等待断开事件后才能回到 IDLE。 */
    BLE_CLIENT_CLEANUP_DISCONNECT,
} ble_client_cleanup_t;

/** 模块唯一共享状态；NimBLE 回调、LVGL 任务和命令任务均可能访问。 */
typedef struct {
    /* LVGL 浏览扫描使用的固定设备表。 */
    ble_client_device_t devices[BLE_CLIENT_MAX_DEVICES];
    size_t device_count;
    /* 设备列表可见内容变化时递增，供 UI 判断数据是否更新。 */
    uint32_t revision;
    /* initialized 表示 Host 已启动；ready 表示 Host 已完成同步。 */
    bool initialized;
    bool ready;
    bool scanning;
    uint8_t own_address_type;
    /* 当前业务模式及 CMD 查询阶段。 */
    ble_client_mode_t mode;
    ble_client_query_phase_t query_phase;
    /* 命中广播后保存连接所需地址，不能固定使用设备 MAC。 */
    ble_addr_t peer_address;
    /* 当前连接和 GATT 发现过程得到的属性句柄。 */
    uint16_t connection_handle;
    uint16_t service_start_handle;
    uint16_t service_end_handle;
    uint16_t device_name_handle;
    /* CMD 查询参数、累积结果及完成回调。 */
    size_t target_name_length;
    ble_client_query_result_t query_result;
    bool query_result_reported;
    ble_client_query_complete_cb_t complete_cb;
    void *complete_context;
} ble_client_state_t;

static const char *TAG = "BLE_CLIENT";
static const ble_uuid16_t s_generic_access_uuid = BLE_UUID16_INIT(0x1800);
static const ble_uuid16_t s_device_name_uuid = BLE_UUID16_INIT(0x2A00);
/* 设备表较大且不用于 DMA，放在 PSRAM；所有跨任务访问都由自旋锁保护。 */
static EXT_RAM_BSS_ATTR ble_client_state_t s_client;
static portMUX_TYPE s_client_lock = portMUX_INITIALIZER_UNLOCKED;

static int gap_event_received(struct ble_gap_event *event, void *argument);
static int service_discovered(uint16_t connection_handle,
                              const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *service,
                              void *argument);
static int characteristic_discovered(uint16_t connection_handle,
                                     const struct ble_gatt_error *error,
                                     const struct ble_gatt_chr *characteristic,
                                     void *argument);
static int device_name_read(uint16_t connection_handle,
                            const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attribute,
                            void *argument);

/**
 * @brief 把 NimBLE 内部的小端地址转换为常见的 MAC 显示顺序。
 *
 * NimBLE 的 val[0] 是地址最右侧字节；协议应答和 UI 使用
 * AA:BB:CC:DD:EE:FF 顺序，因此这里反向复制。
 */
static void copy_display_address(uint8_t destination[6],
                                 const uint8_t nimble_address[6])
{
    for (size_t index = 0; index < 6U; ++index) {
        destination[index] = nimble_address[5U - index];
    }
}

/**
 * @brief 清理动作无法启动时，兜底释放查询操作槽位。
 *
 * 正常情况下取消扫描或断开连接是异步的，状态会停留在 ABORTING，直到收到
 * DISC_COMPLETE 或 DISCONNECT。若清理 API 直接报错，不会再有对应事件，只能
 * 由此函数恢复 IDLE，避免模块永久忙碌。
 */
static void set_idle_after_cleanup(void)
{
    taskENTER_CRITICAL(&s_client_lock);
    if (s_client.mode == BLE_CLIENT_MODE_QUERY &&
        s_client.query_phase == BLE_CLIENT_QUERY_ABORTING) {
        s_client.mode = BLE_CLIENT_MODE_IDLE;
        s_client.query_phase = BLE_CLIENT_QUERY_NONE;
        s_client.connection_handle = BLE_CLIENT_INVALID_CONN_HANDLE;
    }
    taskEXIT_CRITICAL(&s_client_lock);
}

/**
 * @brief 只上报一次查询结果，并根据当前阶段清理扫描或连接。
 *
 * @param communication_succeeded 读取 0x2A00 成功且内容与目标名称完全一致。
 * @param cleanup 回调返回后仍需执行的 BLE 清理动作。
 *
 * 该函数可由 GAP/GATT 回调调用。共享结果先在临界区内标记为已上报并复制到
 * 栈上，离开临界区后才调用 NimBLE API 和业务回调，避免持锁调用外部代码。
 */
static void finish_query(bool communication_succeeded,
                         ble_client_cleanup_t cleanup)
{
    ble_client_query_complete_cb_t complete_cb = NULL;
    void *complete_context = NULL;
    ble_client_query_result_t result = {0};
    uint16_t connection_handle = BLE_CLIENT_INVALID_CONN_HANDLE;

    /* 多个失败事件可能紧邻到达，query_result_reported 保证只回复一次 CMD。 */
    taskENTER_CRITICAL(&s_client_lock);
    if (s_client.mode != BLE_CLIENT_MODE_QUERY ||
        s_client.query_result_reported) {
        taskEXIT_CRITICAL(&s_client_lock);
        return;
    }

    s_client.query_result.communication_succeeded = communication_succeeded;
    s_client.query_result_reported = true;
    s_client.scanning = false;
    complete_cb = s_client.complete_cb;
    complete_context = s_client.complete_context;
    result = s_client.query_result;
    connection_handle = s_client.connection_handle;

    /*
     * 无异步清理时立即释放操作槽位；否则保持 QUERY/ABORTING，防止下一条命令
     * 在旧扫描或旧连接真正结束前启动。
     */
    if (cleanup == BLE_CLIENT_CLEANUP_NONE) {
        s_client.mode = BLE_CLIENT_MODE_IDLE;
        s_client.query_phase = BLE_CLIENT_QUERY_NONE;
        s_client.connection_handle = BLE_CLIENT_INVALID_CONN_HANDLE;
    } else {
        s_client.query_phase = BLE_CLIENT_QUERY_ABORTING;
    }
    taskEXIT_CRITICAL(&s_client_lock);

    ESP_LOGI(TAG,
             "BLE query result: target=%s, scan=%s, communication=%s, "
             "read_name=%s",
             result.target_name,
             result.scan_succeeded ? "success" : "failed",
             result.communication_succeeded ? "success" : "failed",
             result.read_name[0] != '\0' ? result.read_name : "<none>");

    /* GAP 清理 API 只发起动作，最终完成由后续 GAP 事件确认。 */
    int cleanup_result = 0;
    switch (cleanup) {
        case BLE_CLIENT_CLEANUP_CANCEL_SCAN:
            ESP_LOGI(TAG, "Cancelling BLE target scan");
            cleanup_result = ble_gap_disc_cancel();
            break;
        case BLE_CLIENT_CLEANUP_DISCONNECT:
            ESP_LOGI(TAG,
                     "Requesting BLE disconnect: conn_handle=%u",
                     (unsigned)connection_handle);
            cleanup_result = ble_gap_terminate(connection_handle,
                                               BLE_ERR_REM_USER_CONN_TERM);
            break;
        case BLE_CLIENT_CLEANUP_NONE:
        default:
            break;
    }
    if (cleanup != BLE_CLIENT_CLEANUP_NONE && cleanup_result != 0) {
        ESP_LOGW(TAG, "BLE cleanup request failed: rc=%d", cleanup_result);
        set_idle_after_cleanup();
    } else if (cleanup == BLE_CLIENT_CLEANUP_DISCONNECT) {
        ESP_LOGI(TAG,
                 "BLE disconnect request accepted: conn_handle=%u",
                 (unsigned)connection_handle);
    }

    /* 传给回调的是局部完整副本，回调执行期间不依赖共享状态锁。 */
    if (complete_cb != NULL) {
        complete_cb(&result, complete_context);
    }
}

/**
 * @brief 使用统一参数启动一次有限时长的主动扫描。
 *
 * @param duration_ms 本轮扫描持续时间，单位毫秒。
 * @return NimBLE 返回码；0 表示扫描过程已成功启动。
 *
 * 主动扫描会请求 Scan Response，才能更可靠地取得 Complete Local Name。
 * filter_duplicates=0 允许持续更新 RSSI；设备表会自行按地址去重。
 */
static int start_active_scan(uint32_t duration_ms)
{
    uint8_t own_address_type;
    taskENTER_CRITICAL(&s_client_lock);
    own_address_type = s_client.own_address_type;
    taskEXIT_CRITICAL(&s_client_lock);

    const struct ble_gap_disc_params parameters = {
        .itvl = BLE_CLIENT_INTERVAL_UNITS,
        .window = BLE_CLIENT_WINDOW_UNITS,
        .filter_policy = 0,
        .filter_duplicates = 0,
        .limited = 0,
        .passive = 0,
    };
    return ble_gap_disc(own_address_type,
                        (int32_t)duration_ms,
                        &parameters,
                        gap_event_received,
                        NULL);
}

/**
 * @brief 将一条浏览广播报告合并到固定容量设备表。
 *
 * 地址和地址类型共同作为设备键；同一设备的新报告更新名称、RSSI 和最后出现
 * 时间。设备表满时，新地址替换最久没有出现的记录，不进行动态内存分配。
 */
static void update_device(const struct ble_gap_disc_desc *report,
                          const struct ble_hs_adv_fields *fields)
{
    /* 广播字段不是 C 字符串，复制时必须截断并依靠清零缓冲区补 '\0'。 */
    char advertised_name[BLE_CLIENT_NAME_MAX_LENGTH] = {0};
    if (fields->name != NULL && fields->name_len > 0U) {
        size_t name_length = fields->name_len;
        if (name_length >= sizeof(advertised_name)) {
            name_length = sizeof(advertised_name) - 1U;
        }
        memcpy(advertised_name, fields->name, name_length);
    }

    uint8_t address[6];
    copy_display_address(address, report->addr.val);
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    taskENTER_CRITICAL(&s_client_lock);
    /* 扫描停止或切换到 CMD 查询后到达的迟到广播不再写入 UI 设备表。 */
    if (s_client.mode != BLE_CLIENT_MODE_BROWSE) {
        taskEXIT_CRITICAL(&s_client_lock);
        return;
    }

    /* 先按“地址类型 + 地址”查找已有记录，避免重复广播产生重复行。 */
    size_t device_index = s_client.device_count;
    for (size_t index = 0; index < s_client.device_count; ++index) {
        if (s_client.devices[index].address_type == report->addr.type &&
            memcmp(s_client.devices[index].address,
                   address,
                   sizeof(address)) == 0) {
            device_index = index;
            break;
        }
    }

    /* 找不到记录时新增；容量已满则选择 last_seen_ms 最旧的一项覆盖。 */
    bool new_device = false;
    if (device_index == s_client.device_count) {
        new_device = true;
        if (s_client.device_count < BLE_CLIENT_MAX_DEVICES) {
            ++s_client.device_count;
        } else {
            device_index = 0U;
            for (size_t index = 1U; index < s_client.device_count; ++index) {
                if ((uint32_t)(now_ms - s_client.devices[index].last_seen_ms) >
                    (uint32_t)(now_ms -
                               s_client.devices[device_index].last_seen_ms)) {
                    device_index = index;
                }
            }
        }
    }

    ble_client_device_t *device = &s_client.devices[device_index];
    const int8_t previous_rssi = new_device ? 127 : device->rssi;
    if (new_device) {
        memset(device, 0, sizeof(*device));
        memcpy(device->address, address, sizeof(device->address));
        device->address_type = report->addr.type;
    }
    device->rssi = report->rssi;

    /* revision 只在 UI 可见字段变化时递增，单纯更新时间戳不触发刷新。 */
    bool visible_data_changed = new_device;
    if (advertised_name[0] != '\0' &&
        strcmp(device->name, advertised_name) != 0) {
        memcpy(device->name, advertised_name, sizeof(device->name));
        device->name[sizeof(device->name) - 1U] = '\0';
        visible_data_changed = true;
    }
    if (!new_device && device->rssi != previous_rssi) {
        visible_data_changed = true;
    }
    device->last_seen_ms = now_ms;
    if (visible_data_changed) {
        ++s_client.revision;
    }
    taskEXIT_CRITICAL(&s_client_lock);
}

/**
 * @brief 判断广播是否为 CMD 指定目标，并原子地切换到连接阶段。
 *
 * 只接受 Complete Local Name，名称长度和每个字节都必须完全相同。匹配后保存
 * 本次报告中的地址及地址类型，因为 Peripheral 可能使用随机地址；同时保存
 * MAC 显示值和 RSSI，供 CMD 应答使用。
 *
 * @return true 表示本回调首次命中目标，调用者应停止扫描并发起连接。
 */
static bool match_query_target(const struct ble_gap_disc_desc *report,
                               const struct ble_hs_adv_fields *fields)
{
    bool matched = false;
    /* 在锁内检查并改变阶段，避免同一设备的连续广播重复发起连接。 */
    taskENTER_CRITICAL(&s_client_lock);
    if (s_client.mode == BLE_CLIENT_MODE_QUERY &&
        s_client.query_phase == BLE_CLIENT_QUERY_SCANNING &&
        fields->name != NULL && fields->name_is_complete &&
        fields->name_len == s_client.target_name_length &&
        memcmp(fields->name,
               s_client.query_result.target_name,
               s_client.target_name_length) == 0) {
        s_client.peer_address = report->addr;
        copy_display_address(s_client.query_result.address, report->addr.val);
        s_client.query_result.rssi = report->rssi;
        s_client.query_result.scan_succeeded = true;
        s_client.query_phase = BLE_CLIENT_QUERY_CONNECTING;
        s_client.scanning = false;
        matched = true;
    }
    taskEXIT_CRITICAL(&s_client_lock);
    return matched;
}

/**
 * @brief 停止目标扫描，并使用命中广播中的地址发起连接。
 *
 * 必须先取消扫描，再调用 ble_gap_connect()。连接建立最长等待 2 秒，最终结果
 * 通过 BLE_GAP_EVENT_CONNECT 返回；本函数返回 0 并不代表链路已经建立。
 */
static void connect_to_matched_device(void)
{
    ble_addr_t peer_address;
    uint8_t own_address_type;
    int8_t rssi;
    taskENTER_CRITICAL(&s_client_lock);
    peer_address = s_client.peer_address;
    own_address_type = s_client.own_address_type;
    rssi = s_client.query_result.rssi;
    taskEXIT_CRITICAL(&s_client_lock);

    /* 释放扫描过程后再创建 initiator，避免 Controller 活动槽冲突。 */
    const int cancel_result = ble_gap_disc_cancel();
    if (cancel_result != 0) {
        ESP_LOGW(TAG, "Failed to stop target scan: rc=%d", cancel_result);
        finish_query(false, BLE_CLIENT_CLEANUP_CANCEL_SCAN);
        return;
    }

    ESP_LOGI(TAG,
             "Starting BLE connection: peer=%02X:%02X:%02X:%02X:%02X:%02X, "
             "addr_type=%u, RSSI=%d dBm, timeout=%u ms",
             peer_address.val[5],
             peer_address.val[4],
             peer_address.val[3],
             peer_address.val[2],
             peer_address.val[1],
             peer_address.val[0],
             (unsigned)peer_address.type,
             (int)rssi,
             (unsigned)BLE_CLIENT_CONNECT_TIMEOUT_MS);

    /* NULL 连接参数表示采用 NimBLE 默认连接间隔和 supervision timeout。 */
    const int connect_result = ble_gap_connect(
        own_address_type,
        &peer_address,
        BLE_CLIENT_CONNECT_TIMEOUT_MS,
        NULL,
        gap_event_received,
        NULL);
    if (connect_result != 0) {
        ESP_LOGW(TAG, "Failed to start BLE connection: rc=%d", connect_result);
        finish_query(false, BLE_CLIENT_CLEANUP_NONE);
    } else {
        ESP_LOGI(TAG, "BLE connection procedure started");
    }
}

/**
 * @brief 处理 Generic Access Service（UUID 0x1800）发现结果。
 *
 * NimBLE 发现回调可能执行多次：status=0 表示返回一个匹配服务；
 * BLE_HS_EDONE 表示枚举正常结束；其他状态表示失败。枚举结束且已保存服务范围
 * 后，继续在该范围内发现 Device Name 特征。
 */
static int service_discovered(uint16_t connection_handle,
                              const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *service,
                              void *argument)
{
    (void)argument;
    /* 一条有效服务记录：只接收当前连接、当前阶段的回调。 */
    if (error->status == 0 && service != NULL) {
        bool accepted = false;
        taskENTER_CRITICAL(&s_client_lock);
        if (s_client.mode == BLE_CLIENT_MODE_QUERY &&
            s_client.query_phase == BLE_CLIENT_QUERY_DISCOVERING_SERVICE &&
            s_client.connection_handle == connection_handle) {
            s_client.service_start_handle = service->start_handle;
            s_client.service_end_handle = service->end_handle;
            accepted = true;
        }
        taskEXIT_CRITICAL(&s_client_lock);
        if (accepted) {
            ESP_LOGI(TAG,
                     "Generic Access service 0x1800 found: "
                     "conn_handle=%u, range=0x%04X-0x%04X",
                     (unsigned)connection_handle,
                     (unsigned)service->start_handle,
                     (unsigned)service->end_handle);
        }
        return 0;
    }

    /* EDONE 是正常枚举结束，不是错误；其余非零状态均终止查询。 */
    if (error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "Generic Access discovery failed: status=%u",
                 (unsigned)error->status);
        finish_query(false, BLE_CLIENT_CLEANUP_DISCONNECT);
        return 0;
    }

    /*
     * 服务枚举结束后，从共享状态取出句柄范围，同时推进状态。局部变量只在
     * can_continue=true 时使用。
     */
    uint16_t start_handle;
    uint16_t end_handle;
    bool can_continue = false;
    taskENTER_CRITICAL(&s_client_lock);
    if (s_client.mode == BLE_CLIENT_MODE_QUERY &&
        s_client.query_phase == BLE_CLIENT_QUERY_DISCOVERING_SERVICE &&
        s_client.connection_handle == connection_handle &&
        s_client.service_start_handle != 0U) {
        start_handle = s_client.service_start_handle;
        end_handle = s_client.service_end_handle;
        s_client.query_phase = BLE_CLIENT_QUERY_DISCOVERING_CHARACTERISTIC;
        can_continue = true;
    }
    taskEXIT_CRITICAL(&s_client_lock);

    if (!can_continue) {
        ESP_LOGW(TAG, "Generic Access service 0x1800 not found");
        finish_query(false, BLE_CLIENT_CLEANUP_DISCONNECT);
        return 0;
    }

    ESP_LOGI(TAG,
             "Discovering Device Name characteristic 0x2A00: "
             "conn_handle=%u",
             (unsigned)connection_handle);
    /* 只查标准 Device Name UUID，避免遍历服务中的其他特征。 */
    const int result = ble_gattc_disc_chrs_by_uuid(
        connection_handle,
        start_handle,
        end_handle,
        &s_device_name_uuid.u,
        characteristic_discovered,
        NULL);
    if (result != 0) {
        ESP_LOGW(TAG, "Cannot discover Device Name: rc=%d", result);
        finish_query(false, BLE_CLIENT_CLEANUP_DISCONNECT);
    }
    return 0;
}

/**
 * @brief 处理 Device Name Characteristic（UUID 0x2A00）发现结果。
 *
 * 仅保存带 READ 属性的特征值句柄。枚举正常结束后立即读取该句柄；找不到可读
 * 特征或发现过程报错时，返回通信失败并断开连接。
 */
static int characteristic_discovered(
    uint16_t connection_handle,
    const struct ble_gatt_error *error,
    const struct ble_gatt_chr *characteristic,
    void *argument)
{
    (void)argument;
    /* 一条匹配特征记录；仍需确认 READ 属性，不能假定服务端一定配置正确。 */
    if (error->status == 0 && characteristic != NULL) {
        bool accepted = false;
        taskENTER_CRITICAL(&s_client_lock);
        if (s_client.mode == BLE_CLIENT_MODE_QUERY &&
            s_client.query_phase ==
                BLE_CLIENT_QUERY_DISCOVERING_CHARACTERISTIC &&
            s_client.connection_handle == connection_handle &&
            (characteristic->properties & BLE_GATT_CHR_PROP_READ) != 0U) {
            s_client.device_name_handle = characteristic->val_handle;
            accepted = true;
        }
        taskEXIT_CRITICAL(&s_client_lock);
        if (accepted) {
            ESP_LOGI(TAG,
                     "Device Name characteristic 0x2A00 found: "
                     "value_handle=0x%04X, properties=0x%02X",
                     (unsigned)characteristic->val_handle,
                     (unsigned)characteristic->properties);
        }
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "Device Name discovery failed: status=%u",
                 (unsigned)error->status);
        finish_query(false, BLE_CLIENT_CLEANUP_DISCONNECT);
        return 0;
    }

    /* 枚举结束时锁定值句柄，并在发起读取前推进到 READING_NAME。 */
    uint16_t value_handle = 0U;
    taskENTER_CRITICAL(&s_client_lock);
    if (s_client.mode == BLE_CLIENT_MODE_QUERY &&
        s_client.query_phase ==
            BLE_CLIENT_QUERY_DISCOVERING_CHARACTERISTIC &&
        s_client.connection_handle == connection_handle &&
        s_client.device_name_handle != 0U) {
        value_handle = s_client.device_name_handle;
        s_client.query_phase = BLE_CLIENT_QUERY_READING_NAME;
    }
    taskEXIT_CRITICAL(&s_client_lock);

    if (value_handle == 0U) {
        ESP_LOGW(TAG, "Readable Device Name characteristic 0x2A00 not found");
        finish_query(false, BLE_CLIENT_CLEANUP_DISCONNECT);
        return 0;
    }

    ESP_LOGI(TAG,
             "Reading Device Name 0x2A00: conn_handle=%u, value_handle=0x%04X",
             (unsigned)connection_handle,
             (unsigned)value_handle);
    /* 读取结果由 device_name_read() 异步返回。 */
    const int result = ble_gattc_read(connection_handle,
                                      value_handle,
                                      device_name_read,
                                      NULL);
    if (result != 0) {
        ESP_LOGW(TAG, "Cannot read Device Name: rc=%d", result);
        finish_query(false, BLE_CLIENT_CLEANUP_DISCONNECT);
    }
    return 0;
}

/**
 * @brief 处理 0x2A00 的读取结果，按原始字节与目标 SN 精确比较。
 *
 * GATT 数据可能由多个 mbuf 片段组成，先展平到固定缓冲区。只有长度相同且内容
 * 完全一致才判定通信成功；无论匹配与否，本次短连接都在上报结果后主动断开。
 */
static int device_name_read(uint16_t connection_handle,
                            const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attribute,
                            void *argument)
{
    (void)argument;
    if (error->status != 0 || attribute == NULL || attribute->om == NULL) {
        ESP_LOGW(TAG, "Device Name read failed: status=%u",
                 (unsigned)error->status);
        finish_query(false, BLE_CLIENT_CLEANUP_DISCONNECT);
        return 0;
    }

    /* 预留最后一个字节给 '\0'，便于日志和上层把返回值作为 ASCII 字符串使用。 */
    char read_name[BLE_CLIENT_NAME_MAX_LENGTH] = {0};
    uint16_t read_length = 0U;
    const int flatten_result = ble_hs_mbuf_to_flat(
        attribute->om,
        read_name,
        sizeof(read_name) - 1U,
        &read_length);
    if (flatten_result != 0) {
        ESP_LOGW(TAG, "Cannot copy Device Name: rc=%d", flatten_result);
        finish_query(false, BLE_CLIENT_CLEANUP_DISCONNECT);
        return 0;
    }

    /* 仅当前连接处于读取阶段时接受结果，忽略旧连接的迟到回调。 */
    bool names_match = false;
    char target_name[BLE_CLIENT_TARGET_NAME_MAX_LENGTH + 1U] = {0};
    taskENTER_CRITICAL(&s_client_lock);
    if (s_client.mode == BLE_CLIENT_MODE_QUERY &&
        s_client.query_phase == BLE_CLIENT_QUERY_READING_NAME &&
        s_client.connection_handle == connection_handle) {
        memcpy(s_client.query_result.read_name, read_name, read_length);
        s_client.query_result.read_name[read_length] = '\0';
        memcpy(target_name,
               s_client.query_result.target_name,
               sizeof(target_name));
        names_match = read_length == s_client.target_name_length &&
                      memcmp(read_name,
                             s_client.query_result.target_name,
                             read_length) == 0;
    }
    taskEXIT_CRITICAL(&s_client_lock);

    ESP_LOGI(TAG,
             "Device Name read: value=%s, expected=%s, match=%s",
             read_name,
             target_name,
             names_match ? "yes" : "no");
    finish_query(names_match, BLE_CLIENT_CLEANUP_DISCONNECT);
    return 0;
}

/**
 * @brief NimBLE GAP 统一事件入口。
 *
 * 本函数运行在 NimBLE Host 任务中，负责把扫描报告、扫描完成、连接完成和断开
 * 事件推进到模块状态机。回调内不操作 LVGL，也不直接组协议帧；最终结果通过
 * complete_cb 交给命令模块。
 */
static int gap_event_received(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            /*
             * 每收到一包 Legacy 广播或 Scan Response 都会进入这里。先用 NimBLE
             * 解析 AD Structure，再根据当前业务模式更新列表或匹配目标名称。
             */
            struct ble_hs_adv_fields fields = {0};
            if (ble_hs_adv_parse_fields(&fields,
                                        event->disc.data,
                                        event->disc.length_data) != 0) {
                return 0;
            }

            /* 锁内只读取模式；解析广播和后续 BLE API 调用均在锁外完成。 */
            ble_client_mode_t mode;
            taskENTER_CRITICAL(&s_client_lock);
            mode = s_client.mode;
            taskEXIT_CRITICAL(&s_client_lock);
            if (mode == BLE_CLIENT_MODE_BROWSE) {
                update_device(&event->disc, &fields);
            } else if (mode == BLE_CLIENT_MODE_QUERY &&
                       match_query_target(&event->disc, &fields)) {
                ESP_LOGI(TAG,
                         "Target BLE name found: %.*s, "
                         "peer=%02X:%02X:%02X:%02X:%02X:%02X, "
                         "addr_type=%u, RSSI=%d dBm",
                         (int)fields.name_len,
                         (const char *)fields.name,
                         event->disc.addr.val[5],
                         event->disc.addr.val[4],
                         event->disc.addr.val[3],
                         event->disc.addr.val[2],
                         event->disc.addr.val[1],
                         event->disc.addr.val[0],
                         (unsigned)event->disc.addr.type,
                         (int)event->disc.rssi);
                connect_to_matched_device();
            }
            break;
        }

        case BLE_GAP_EVENT_DISC_COMPLETE: {
            /*
             * 有限时长到期或主动取消都会触发扫描完成。先保存事件到达时的模式
             * 和阶段，因为 finish_query() 会继续改变共享状态。
             */
            ble_client_mode_t mode;
            ble_client_query_phase_t phase;
            size_t device_count;
            taskENTER_CRITICAL(&s_client_lock);
            s_client.scanning = false;
            mode = s_client.mode;
            phase = s_client.query_phase;
            device_count = s_client.device_count;
            if (mode == BLE_CLIENT_MODE_BROWSE) {
                /* LVGL 浏览只保留本轮快照，随后释放 BLE 操作槽位。 */
                s_client.mode = BLE_CLIENT_MODE_IDLE;
            } else if (mode == BLE_CLIENT_MODE_QUERY &&
                       phase == BLE_CLIENT_QUERY_ABORTING) {
                /* 查询失败后发起的扫描取消已完成，可以接受下一条请求。 */
                s_client.mode = BLE_CLIENT_MODE_IDLE;
                s_client.query_phase = BLE_CLIENT_QUERY_NONE;
            }
            taskEXIT_CRITICAL(&s_client_lock);

            if (mode == BLE_CLIENT_MODE_BROWSE) {
                ESP_LOGI(TAG,
                         "BLE browse scan complete: reason=%d, devices=%u",
                         event->disc_complete.reason,
                         (unsigned)device_count);
            } else if (mode == BLE_CLIENT_MODE_QUERY &&
                       phase == BLE_CLIENT_QUERY_SCANNING) {
                /* 完整 5 秒扫描结束仍处于 SCANNING，说明没有命中目标名称。 */
                ESP_LOGW(TAG,
                         "BLE target scan complete: target not found, reason=%d",
                         event->disc_complete.reason);
                finish_query(false, BLE_CLIENT_CLEANUP_NONE);
            } else if (mode == BLE_CLIENT_MODE_QUERY &&
                       phase == BLE_CLIENT_QUERY_CONNECTING) {
                /* 命中目标后主动取消扫描；连接过程已经由调用处发起。 */
                ESP_LOGI(TAG, "BLE target scan stopped after target match");
            } else if (mode == BLE_CLIENT_MODE_QUERY &&
                       phase == BLE_CLIENT_QUERY_ABORTING) {
                ESP_LOGI(TAG, "BLE target scan cancelled");
            }
            break;
        }

        case BLE_GAP_EVENT_CONNECT: {
            /* ble_gap_connect() 的异步最终结果，包括 2 秒连接超时。 */
            ble_client_query_phase_t phase;
            taskENTER_CRITICAL(&s_client_lock);
            phase = s_client.query_phase;
            taskEXIT_CRITICAL(&s_client_lock);

            if (phase == BLE_CLIENT_QUERY_ABORTING) {
                /*
                 * 查询结果已经上报但连接事件迟到。若链路意外建立，立即断开，
                 * 防止留下无人管理的连接。
                 */
                if (event->connect.status == 0) {
                    ESP_LOGW(TAG,
                             "BLE connection completed after query abort; "
                             "disconnecting: conn_handle=%u",
                             (unsigned)event->connect.conn_handle);
                    const int result = ble_gap_terminate(
                        event->connect.conn_handle,
                        BLE_ERR_REM_USER_CONN_TERM);
                    if (result != 0) {
                        set_idle_after_cleanup();
                    }
                } else {
                    set_idle_after_cleanup();
                }
                break;
            }

            /* 非本轮查询的连接事件不能推进 GATT 状态机。 */
            if (phase != BLE_CLIENT_QUERY_CONNECTING) {
                break;
            }
            if (event->connect.status != 0) {
                ESP_LOGW(TAG, "BLE connection failed: status=%d",
                         event->connect.status);
                finish_query(false, BLE_CLIENT_CLEANUP_NONE);
                break;
            }

            ESP_LOGI(TAG,
                     "BLE connected: conn_handle=%u; discovering "
                     "Generic Access service 0x1800",
                     (unsigned)event->connect.conn_handle);
            /* 连接建立后清空上一轮句柄，并进入服务发现阶段。 */
            taskENTER_CRITICAL(&s_client_lock);
            s_client.connection_handle = event->connect.conn_handle;
            s_client.service_start_handle = 0U;
            s_client.service_end_handle = 0U;
            s_client.device_name_handle = 0U;
            s_client.query_phase = BLE_CLIENT_QUERY_DISCOVERING_SERVICE;
            taskEXIT_CRITICAL(&s_client_lock);

            /* 只发现标准 Generic Access 服务，不做全量服务遍历。 */
            const int result = ble_gattc_disc_svc_by_uuid(
                event->connect.conn_handle,
                &s_generic_access_uuid.u,
                service_discovered,
                NULL);
            if (result != 0) {
                ESP_LOGW(TAG, "Cannot discover Generic Access: rc=%d", result);
                finish_query(false, BLE_CLIENT_CLEANUP_DISCONNECT);
            }
            break;
        }

        case BLE_GAP_EVENT_DISCONNECT: {
            /* 主动断开和对端断开都从这里完成状态回收。 */
            ESP_LOGI(TAG,
                     "BLE disconnected: conn_handle=%u, reason=%d",
                     (unsigned)event->disconnect.conn.conn_handle,
                     event->disconnect.reason);
            bool query_failed = false;
            taskENTER_CRITICAL(&s_client_lock);
            if (s_client.mode == BLE_CLIENT_MODE_QUERY &&
                s_client.query_phase == BLE_CLIENT_QUERY_ABORTING) {
                /* finish_query() 已上报结果，本事件只负责释放操作槽位。 */
                s_client.mode = BLE_CLIENT_MODE_IDLE;
                s_client.query_phase = BLE_CLIENT_QUERY_NONE;
                s_client.connection_handle = BLE_CLIENT_INVALID_CONN_HANDLE;
            } else if (s_client.mode == BLE_CLIENT_MODE_QUERY &&
                       !s_client.query_result_reported) {
                /* GATT 完成前对端断开，本次通信直接判定失败。 */
                query_failed = true;
            }
            taskEXIT_CRITICAL(&s_client_lock);
            if (query_failed) {
                finish_query(false, BLE_CLIENT_CLEANUP_NONE);
            }
            break;
        }

        default:
            break;
    }
    return 0;
}

/**
 * @brief NimBLE Host 与 Controller 同步完成回调。
 *
 * 创建或确认本机 Identity Address，并推断 Central 发起扫描/连接时应使用的地址
 * 类型。只有执行到这里后 ready 才为 true，外部请求才能启动。
 */
static void host_synchronized(void)
{
    int result = ble_hs_util_ensure_addr(0);
    uint8_t own_address_type = 0U;
    if (result == 0) {
        result = ble_hs_id_infer_auto(0, &own_address_type);
    }
    if (result != 0) {
        ESP_LOGE(TAG, "Cannot create BLE identity address: rc=%d", result);
        return;
    }

    taskENTER_CRITICAL(&s_client_lock);
    s_client.ready = true;
    s_client.own_address_type = own_address_type;
    taskEXIT_CRITICAL(&s_client_lock);
    ESP_LOGI(TAG, "BLE Central / GATT Client ready; waiting for a request");
}

/**
 * @brief NimBLE Host 异常复位回调。
 *
 * 复位后立即禁止新请求；若 CMD 查询仍未上报，则以通信失败结束，避免调用者
 * 一直等待。后续 Host 再次同步时 host_synchronized() 会恢复 ready。
 */
static void host_reset(int reason)
{
    bool query_active;
    taskENTER_CRITICAL(&s_client_lock);
    s_client.ready = false;
    s_client.scanning = false;
    query_active = s_client.mode == BLE_CLIENT_MODE_QUERY &&
                   !s_client.query_result_reported;
    if (!query_active) {
        s_client.mode = BLE_CLIENT_MODE_IDLE;
        s_client.query_phase = BLE_CLIENT_QUERY_NONE;
    }
    taskEXIT_CRITICAL(&s_client_lock);
    ESP_LOGE(TAG, "NimBLE host reset: reason=%d", reason);
    if (query_active) {
        finish_query(false, BLE_CLIENT_CLEANUP_NONE);
    }
}

/** @brief FreeRTOS NimBLE Host 任务入口，事件循环退出后释放端口任务资源。 */
static void host_task(void *argument)
{
    (void)argument;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/**
 * @brief 初始化 NimBLE Central/GATT Client，并启动 Host 任务。
 *
 * 此函数可重复调用；第一次调用只代表 Host 已启动，真正可扫描要等同步回调把
 * ready 置位。函数不会自动扫描，也不会建立连接。
 */
esp_err_t ble_client_init(void)
{
    taskENTER_CRITICAL(&s_client_lock);
    const bool already_initialized = s_client.initialized;
    taskEXIT_CRITICAL(&s_client_lock);
    if (already_initialized) {
        return ESP_OK;
    }

    /* 初始化 NimBLE Port、Host 协议栈及其内部资源。 */
    esp_err_t result = nimble_port_init();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Cannot initialize NimBLE: %s", esp_err_to_name(result));
        return result;
    }

    /* 在启动 Host 任务前注册复位和同步回调。 */
    ble_hs_cfg.reset_cb = host_reset;
    ble_hs_cfg.sync_cb = host_synchronized;
    taskENTER_CRITICAL(&s_client_lock);
    s_client.initialized = true;
    s_client.mode = BLE_CLIENT_MODE_IDLE;
    s_client.connection_handle = BLE_CLIENT_INVALID_CONN_HANDLE;
    taskEXIT_CRITICAL(&s_client_lock);
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

/**
 * @brief 为 LVGL 页面启动一次 5 秒浏览扫描。
 *
 * 每轮开始时清空旧设备表。若 CMD 查询或上一轮扫描仍占用模块，则返回
 * ESP_ERR_INVALID_STATE；页面定时器会稍后重试，不会抢占 CMD 操作。
 */
esp_err_t ble_client_start_scan(void)
{
    /* 检查并占用唯一操作槽位，防止 LVGL 扫描与 CMD 查询并发使用 GAP。 */
    taskENTER_CRITICAL(&s_client_lock);
    if (!s_client.initialized || !s_client.ready) {
        taskEXIT_CRITICAL(&s_client_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_client.mode != BLE_CLIENT_MODE_IDLE) {
        taskEXIT_CRITICAL(&s_client_lock);
        return ESP_ERR_INVALID_STATE;
    }

    /* 每轮展示独立的附近设备快照，不保留上一轮已经消失的设备。 */
    memset(s_client.devices, 0, sizeof(s_client.devices));
    s_client.device_count = 0U;
    ++s_client.revision;
    s_client.mode = BLE_CLIENT_MODE_BROWSE;
    s_client.scanning = true;
    taskEXIT_CRITICAL(&s_client_lock);

    /* 状态先置为 BROWSE，确保紧接着到达的广播回调能正确写入设备表。 */
    const int result = start_active_scan(BLE_CLIENT_BROWSE_DURATION_MS);
    if (result != 0) {
        taskENTER_CRITICAL(&s_client_lock);
        s_client.mode = BLE_CLIENT_MODE_IDLE;
        s_client.scanning = false;
        taskEXIT_CRITICAL(&s_client_lock);
        ESP_LOGW(TAG, "Cannot start BLE browse scan: rc=%d", result);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "BLE browse scan started: duration=%u ms, interval=%u ms, "
             "window=%u ms",
             (unsigned)BLE_CLIENT_BROWSE_DURATION_MS,
             (unsigned)BLE_CLIENT_INTERVAL_MS,
             (unsigned)BLE_CLIENT_WINDOW_MS);
    return ESP_OK;
}

/**
 * @brief 为 CMD 0x02 启动“扫描名称 -> 连接 -> 读取 SN”异步查询。
 *
 * @param target_name 广播 Complete Local Name，同时也是期望从 0x2A00 读回的值。
 * @param complete_cb 查询结束回调，每个成功启动的请求保证最多调用一次。
 * @param context 原样传回 complete_cb 的调用者上下文。
 * @return ESP_OK 表示查询已经启动；参数错误或 BLE 忙时立即返回错误。
 */
esp_err_t ble_client_query_device(
    const char *target_name,
    ble_client_query_complete_cb_t complete_cb,
    void *context)
{
    /* CMD 名称字段最多 20 字节；strnlen 上限多 1 字节用于检测超长输入。 */
    if (target_name == NULL || complete_cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t target_length = strnlen(
        target_name,
        BLE_CLIENT_TARGET_NAME_MAX_LENGTH + 1U);
    if (target_length == 0U ||
        target_length > BLE_CLIENT_TARGET_NAME_MAX_LENGTH) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 检查 Host 状态并原子占用操作槽位。 */
    taskENTER_CRITICAL(&s_client_lock);
    if (!s_client.initialized || !s_client.ready ||
        s_client.mode != BLE_CLIENT_MODE_IDLE) {
        taskEXIT_CRITICAL(&s_client_lock);
        return ESP_ERR_INVALID_STATE;
    }

    /* 为新请求清空所有结果与上一条连接留下的 GATT 句柄。 */
    memset(&s_client.query_result, 0, sizeof(s_client.query_result));
    memcpy(s_client.query_result.target_name, target_name, target_length);
    s_client.query_result.target_name[target_length] = '\0';
    s_client.query_result.rssi = 127;
    s_client.target_name_length = target_length;
    s_client.query_result_reported = false;
    s_client.complete_cb = complete_cb;
    s_client.complete_context = context;
    s_client.connection_handle = BLE_CLIENT_INVALID_CONN_HANDLE;
    s_client.service_start_handle = 0U;
    s_client.service_end_handle = 0U;
    s_client.device_name_handle = 0U;
    s_client.mode = BLE_CLIENT_MODE_QUERY;
    s_client.query_phase = BLE_CLIENT_QUERY_SCANNING;
    s_client.scanning = true;
    taskEXIT_CRITICAL(&s_client_lock);

    /* 状态先进入 SCANNING，广播事件才会执行精确名称匹配。 */
    const int scan_result = start_active_scan(
        BLE_CLIENT_QUERY_SCAN_DURATION_MS);
    if (scan_result != 0) {
        taskENTER_CRITICAL(&s_client_lock);
        s_client.mode = BLE_CLIENT_MODE_IDLE;
        s_client.query_phase = BLE_CLIENT_QUERY_NONE;
        s_client.scanning = false;
        taskEXIT_CRITICAL(&s_client_lock);
        ESP_LOGW(TAG, "Cannot start BLE target scan: rc=%d", scan_result);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "BLE target scan started: target=%s, duration=%u ms, "
             "interval=%u ms, window=%u ms",
             target_name,
             (unsigned)BLE_CLIENT_QUERY_SCAN_DURATION_MS,
             (unsigned)BLE_CLIENT_INTERVAL_MS,
             (unsigned)BLE_CLIENT_WINDOW_MS);
    return ESP_OK;
}

/**
 * @brief 将当前浏览设备表复制为调用者私有快照。
 *
 * LVGL 不直接持有 s_client 内部指针。整张表在临界区内复制，保证 device_count
 * 和每条设备数据属于同一状态；返回后 UI 可在无锁状态下排序和渲染。
 */
esp_err_t ble_client_get_snapshot(ble_client_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(snapshot, 0, sizeof(*snapshot));

    taskENTER_CRITICAL(&s_client_lock);
    if (!s_client.initialized) {
        taskEXIT_CRITICAL(&s_client_lock);
        return ESP_ERR_INVALID_STATE;
    }

    snapshot->ready = s_client.ready;
    snapshot->scanning = s_client.scanning;
    snapshot->revision = s_client.revision;
    for (size_t index = 0; index < s_client.device_count; ++index) {
        const ble_client_device_t *device = &s_client.devices[index];
        snapshot->devices[snapshot->device_count] = *device;
        ++snapshot->device_count;
    }
    taskEXIT_CRITICAL(&s_client_lock);
    return ESP_OK;
}
