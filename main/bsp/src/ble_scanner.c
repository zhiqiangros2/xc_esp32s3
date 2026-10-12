/**
 * @file ble_scanner.c
 * @brief ESP32-S3 NimBLE 常驻扫描服务。
 *
 * ESP32-S3 只支持 BLE，不支持经典蓝牙。该模块只启用 Observer 角色：
 *
 *   BLE 控制器收到广播/扫描响应
 *                 |
 *                 v
 *       NimBLE GAP 事件回调
 *                 |
 *                 v
 *   按 address_type + MAC 查找设备
 *                 |
 *        +--------+--------+
 *        |                 |
 *      已存在             新设备
 *   更新名称/RSSI      使用空槽或替换最旧项
 *        |                 |
 *        +--------+--------+
 *                 |
 *                 v
 *        BSP 内部固定设备表
 *                 |
 *                 v
 *   ble_scanner_get_snapshot() 加锁复制
 *                 |
 *          +------+------+
 *          |             |
 *        LVGL UI       后续业务接口
 *
 * UI 页面关闭后 NimBLE 主机任务和扫描过程都继续运行，所以再次打开页面时可以
 * 立即取得已有结果。设备超过 10 秒没有广播时不会出现在快照中；内部槽位仍可在
 * 设备再次出现时复用，表满后优先替换最久未出现的设备。
 */
#include "ble_scanner.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

/*
 * BLE 扫描时间轴：
 *
 *   |<---------- interval：200 ms ---------->|
 *   |<-- window：100 ms -->|<-- 空闲 100 ms -->|
 *   |      监听广播         |   暂停射频监听     |
 *
 * interval 是相邻两个扫描周期起点之间的时间，不是“扫描持续 200 ms”；
 * window 是每个 interval 内真正监听 BLE 广播的时间，且必须小于或等于
 * interval。当前占空比为 100 / 200 = 50%。
 *
 * 控制器每 200 ms 监听 100 ms。UI 每 2 秒读取一次快照，因此每次 UI 刷新
 * 之间约有 10 个扫描窗口；相比原来的 100% 连续监听，可以降低射频和控制器
 * 负载，同时仍能较快取得附近设备的新广播。BLE_HS_FOREVER 只表示扫描服务
 * 不会自动结束，与这里的扫描间隔和窗口不是同一个概念。
 *
 * NimBLE 的 itvl 和 window 字段不使用毫秒，而使用 0.625 ms（625 us）为单位：
 *
 *   200 ms / 0.625 ms = 320 units
 *   100 ms / 0.625 ms = 160 units
 */
/* 人类可读的扫描周期，仅用于说明和启动日志。 */
#define BLE_SCANNER_INTERVAL_MS 200U
/* 人类可读的单周期监听时间，仅用于说明和启动日志。 */
#define BLE_SCANNER_WINDOW_MS 100U
/* 传给 ble_gap_disc() 的 itvl：320 x 0.625 ms = 200 ms。 */
#define BLE_SCANNER_INTERVAL_UNITS 320U
/* 传给 ble_gap_disc() 的 window：160 x 0.625 ms = 100 ms。 */
#define BLE_SCANNER_WINDOW_UNITS 160U
/* 超过该时间没有再次收到广播的设备不再返回给调用者。 */
#define BLE_SCANNER_DEVICE_TIMEOUT_MS 10000U

typedef struct {
    ble_scanner_device_t devices[BLE_SCANNER_MAX_DEVICES];
    size_t device_count;
    uint32_t revision;
    bool initialized;
    bool ready;
    bool scanning;
} ble_scanner_state_t;

static const char *TAG = "BLE_SCANNER";
/*
 * 设备表只由普通任务访问，并在临界区内复制，不参与 ISR 或 DMA；固定容量表放到
 * PSRAM，避免 32 台设备的名称、地址和时间戳长期占用内部 RAM。同步锁仍在内部。
 */
static EXT_RAM_BSS_ATTR ble_scanner_state_t s_scanner;

/* NimBLE 主机任务和其他调用任务会并发访问 s_scanner。 */
static portMUX_TYPE s_scanner_lock = portMUX_INITIALIZER_UNLOCKED;

static int gap_event_received(struct ble_gap_event *event, void *argument);

/** 将 NimBLE 的低字节在前地址转换为常见的 AA:BB:CC:DD:EE:FF 顺序。 */
static void copy_display_address(uint8_t destination[6],
                                 const uint8_t nimble_address[6])
{
    for (size_t index = 0; index < 6U; ++index) {
        destination[index] = nimble_address[5U - index];
    }
}

/**
 * 把一次广播报告合并到固定设备表。
 *
 * 主广播包可能没有名称，而同一设备随后返回的扫描响应带有名称，因此空名称不会
 * 覆盖此前已经保存的名称。RSSI 不做平滑或换算，直接保存当前广播报告中的原始值。
 */
static void update_device(const struct ble_gap_disc_desc *report)
{
    struct ble_hs_adv_fields fields = {0};
    char advertised_name[BLE_SCANNER_NAME_MAX_LENGTH] = {0};
    if (ble_hs_adv_parse_fields(&fields,
                                report->data,
                                report->length_data) == 0 &&
        fields.name != NULL && fields.name_len > 0U) {
        size_t name_length = fields.name_len;
        if (name_length >= sizeof(advertised_name)) {
            name_length = sizeof(advertised_name) - 1U;
        }
        memcpy(advertised_name, fields.name, name_length);
        advertised_name[name_length] = '\0';
    }

    uint8_t address[6];
    copy_display_address(address, report->addr.val);
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    taskENTER_CRITICAL(&s_scanner_lock);

    size_t device_index = s_scanner.device_count;
    for (size_t index = 0; index < s_scanner.device_count; ++index) {
        if (s_scanner.devices[index].address_type == report->addr.type &&
            memcmp(s_scanner.devices[index].address,
                   address,
                   sizeof(address)) == 0) {
            device_index = index;
            break;
        }
    }

    bool new_device = false;
    if (device_index == s_scanner.device_count) {
        new_device = true;
        if (s_scanner.device_count < BLE_SCANNER_MAX_DEVICES) {
            ++s_scanner.device_count;
        } else {
            device_index = 0;
            for (size_t index = 1; index < s_scanner.device_count; ++index) {
                if ((uint32_t)(now_ms -
                               s_scanner.devices[index].last_seen_ms) >
                    (uint32_t)(now_ms -
                               s_scanner.devices[device_index].last_seen_ms)) {
                    device_index = index;
                }
            }
        }
    }

    ble_scanner_device_t *device = &s_scanner.devices[device_index];
    const int8_t previous_rssi = new_device ? 127 : device->rssi;
    if (new_device) {
        memset(device, 0, sizeof(*device));
        memcpy(device->address, address, sizeof(device->address));
        device->address_type = report->addr.type;
    }
    device->rssi = report->rssi;

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
        ++s_scanner.revision;
    }

    taskEXIT_CRITICAL(&s_scanner_lock);
}

/** 启动无限期主动扫描；主动扫描可以取得设备放在扫描响应中的本地名称。 */
static void start_scan(void)
{
    uint8_t own_address_type;
    int result = ble_hs_id_infer_auto(0, &own_address_type);
    if (result != 0) {
        ESP_LOGE(TAG, "Cannot determine BLE address type: rc=%d", result);
        return;
    }

    const struct ble_gap_disc_params parameters = {
        .itvl = BLE_SCANNER_INTERVAL_UNITS,
        .window = BLE_SCANNER_WINDOW_UNITS,
        .filter_policy = 0,
        /* 不过滤重复包，才能持续刷新 RSSI 和最后出现时间。 */
        .filter_duplicates = 0,
        .limited = 0,
        /* passive=0 表示主动扫描，会请求扫描响应以获得更多名称。 */
        .passive = 0,
    };

    result = ble_gap_disc(own_address_type,
                          BLE_HS_FOREVER,
                          &parameters,
                          gap_event_received,
                          NULL);

    taskENTER_CRITICAL(&s_scanner_lock);
    s_scanner.scanning = result == 0;
    taskEXIT_CRITICAL(&s_scanner_lock);

    if (result == 0) {
        ESP_LOGI(TAG,
                 "Background active BLE scan started: interval=%u ms, "
                 "window=%u ms, duty=50%%",
                 (unsigned)BLE_SCANNER_INTERVAL_MS,
                 (unsigned)BLE_SCANNER_WINDOW_MS);
    } else {
        ESP_LOGE(TAG, "Cannot start BLE scan: rc=%d", result);
    }
}

/** NimBLE GAP 回调运行在主机任务中，只更新设备表，不直接操作 LVGL。 */
static int gap_event_received(struct ble_gap_event *event, void *argument)
{
    (void)argument;

    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            update_device(&event->disc);
            break;

        case BLE_GAP_EVENT_DISC_COMPLETE:
            taskENTER_CRITICAL(&s_scanner_lock);
            s_scanner.scanning = false;
            taskEXIT_CRITICAL(&s_scanner_lock);
            ESP_LOGW(TAG,
                     "BLE scan stopped, restarting: reason=%d",
                     event->disc_complete.reason);
            start_scan();
            break;

        default:
            break;
    }
    return 0;
}

/** NimBLE 控制器和主机同步完成后建立地址并开始扫描。 */
static void host_synchronized(void)
{
    const int result = ble_hs_util_ensure_addr(0);
    if (result != 0) {
        ESP_LOGE(TAG, "Cannot create BLE identity address: rc=%d", result);
        return;
    }

    taskENTER_CRITICAL(&s_scanner_lock);
    s_scanner.ready = true;
    taskEXIT_CRITICAL(&s_scanner_lock);
    start_scan();
}

/** 控制器异常复位时保留设备快照，但先标记服务尚未恢复。 */
static void host_reset(int reason)
{
    taskENTER_CRITICAL(&s_scanner_lock);
    s_scanner.ready = false;
    s_scanner.scanning = false;
    taskEXIT_CRITICAL(&s_scanner_lock);
    ESP_LOGE(TAG, "NimBLE host reset: reason=%d", reason);
}

/** NimBLE 自己的常驻主机任务；只有显式停止协议栈时 nimble_port_run() 才返回。 */
static void host_task(void *argument)
{
    (void)argument;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_scanner_init(void)
{
    taskENTER_CRITICAL(&s_scanner_lock);
    const bool already_initialized = s_scanner.initialized;
    taskEXIT_CRITICAL(&s_scanner_lock);
    if (already_initialized) {
        return ESP_OK;
    }

    const esp_err_t result = nimble_port_init();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Cannot initialize NimBLE: %s", esp_err_to_name(result));
        return result;
    }

    ble_hs_cfg.reset_cb = host_reset;
    ble_hs_cfg.sync_cb = host_synchronized;

    taskENTER_CRITICAL(&s_scanner_lock);
    s_scanner.initialized = true;
    taskEXIT_CRITICAL(&s_scanner_lock);

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

esp_err_t ble_scanner_get_snapshot(ble_scanner_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(snapshot, 0, sizeof(*snapshot));

    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    taskENTER_CRITICAL(&s_scanner_lock);
    if (!s_scanner.initialized) {
        taskEXIT_CRITICAL(&s_scanner_lock);
        return ESP_ERR_INVALID_STATE;
    }

    snapshot->ready = s_scanner.ready;
    snapshot->scanning = s_scanner.scanning;
    snapshot->revision = s_scanner.revision;
    for (size_t index = 0; index < s_scanner.device_count; ++index) {
        const ble_scanner_device_t *device = &s_scanner.devices[index];
        if ((uint32_t)(now_ms - device->last_seen_ms) >
            BLE_SCANNER_DEVICE_TIMEOUT_MS) {
            continue;
        }
        snapshot->devices[snapshot->device_count] = *device;
        ++snapshot->device_count;
    }
    taskEXIT_CRITICAL(&s_scanner_lock);
    return ESP_OK;
}
