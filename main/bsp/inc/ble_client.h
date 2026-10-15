#ifndef BSP_BLE_CLIENT_H
#define BSP_BLE_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* 固定容量避免扫描回调动态分配内存，方便 UI 取得完整快照。 */
#define BLE_CLIENT_MAX_DEVICES 32U
/* 一轮 UI 浏览扫描持续时间。 */
#define BLE_CLIENT_BROWSE_DURATION_MS 5000U
/* Legacy BLE 广播中的本地名称通常不超过 29 字节，这里额外留出扩展空间。 */
#define BLE_CLIENT_NAME_MAX_LENGTH 40U
/* CMD 0x02 的目标蓝牙名称字段固定占 20 字节。 */
#define BLE_CLIENT_TARGET_NAME_MAX_LENGTH 20U

/** 一台近期仍在发送广播的 BLE 设备。 */
typedef struct {
    /* 按常见显示顺序保存，address[0] 是 MAC 地址最左侧的字节。 */
    uint8_t address[6];
    uint8_t address_type;
    /* 广播未携带本地名称时 name[0] 为 '\0'，调用者可显示“未命名”。 */
    char name[BLE_CLIENT_NAME_MAX_LENGTH];
    /* 最近一次广播报告的原始 RSSI，单位 dBm；越接近 0 表示信号越强。 */
    int8_t rssi;
    /* 自系统启动以来最后收到该 MAC 广播的毫秒时间。 */
    uint32_t last_seen_ms;
} ble_client_device_t;

/**
 * 设备表的一致性副本。
 *
 * NimBLE 回调只修改 BSP 内部设备表，UI 和以后增加的业务接口通过本结构读取，
 * 不会持有扫描任务正在修改的内存。
 */
typedef struct {
    bool ready;
    bool scanning;
    uint32_t revision;
    size_t device_count;
    ble_client_device_t devices[BLE_CLIENT_MAX_DEVICES];
} ble_client_snapshot_t;

/** 按名称搜索、连接并读取 Device Name 后的结果。 */
typedef struct {
    bool scan_succeeded;
    bool communication_succeeded;
    uint8_t address[6];
    /* 广播报告中的原始 RSSI，单位 dBm。 */
    int8_t rssi;
    char target_name[BLE_CLIENT_TARGET_NAME_MAX_LENGTH + 1U];
    char read_name[BLE_CLIENT_NAME_MAX_LENGTH];
} ble_client_query_result_t;

/** 查询结束回调；在 NimBLE 主机任务或超时定时器任务中调用。 */
typedef void (*ble_client_query_complete_cb_t)(
    const ble_client_query_result_t *result,
    void *context);

/**
 * @brief 初始化 NimBLE Central / GATT Client。
 *
 * 必须在 nvs_flash_init() 成功后调用。函数只启动 NimBLE 主机任务，不自动
 * 扫描；后续由 UI 浏览请求或 CMD 0x02 的目标查询请求启动扫描。
 */
esp_err_t ble_client_init(void);

/**
 * @brief 启动一次只扫描、不连接的 5 秒主动扫描，供 LVGL 页面显示设备列表。
 *
 * 每次启动都会清空上一次浏览结果。同一时刻只能执行一个浏览或目标查询请求。
 */
esp_err_t ble_client_start_scan(void);

/**
 * @brief 按 Complete Local Name 搜索设备并读取 0x1800/0x2A00。
 *
 * 目标扫描最长 5 秒，建立连接最长 2 秒。找到名称后使用本次广播中的地址和地址
 * 类型连接，不固定 MAC；读取值与 target_name 完全相同时
 * communication_succeeded 才为 true。
 */
esp_err_t ble_client_query_device(
    const char *target_name,
    ble_client_query_complete_cb_t complete_cb,
    void *context);

/**
 * @brief 复制当前 BLE 设备快照。
 *
 * 返回本次 UI 浏览扫描得到的设备，扫描结束后保留结果到下一次浏览扫描。函数可
 * 由任意任务调用，内部临界区保证名称、MAC 和 RSSI 来自同一时刻。
 */
esp_err_t ble_client_get_snapshot(ble_client_snapshot_t *snapshot);

#endif
