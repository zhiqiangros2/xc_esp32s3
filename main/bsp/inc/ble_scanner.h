#ifndef BSP_BLE_SCANNER_H
#define BSP_BLE_SCANNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* 固定容量避免扫描回调动态分配内存，方便 UI 和其他模块取得完整快照。 */
#define BLE_SCANNER_MAX_DEVICES 32U
/* Legacy BLE 广播中的本地名称通常不超过 29 字节，这里额外留出扩展空间。 */
#define BLE_SCANNER_NAME_MAX_LENGTH 40U

/** 一台近期仍在发送广播的 BLE 设备。 */
typedef struct {
    /* 按常见显示顺序保存，address[0] 是 MAC 地址最左侧的字节。 */
    uint8_t address[6];
    uint8_t address_type;
    /* 广播未携带本地名称时 name[0] 为 '\0'，调用者可显示“未命名”。 */
    char name[BLE_SCANNER_NAME_MAX_LENGTH];
    /* 最近一次广播报告的原始 RSSI，单位 dBm；越接近 0 表示信号越强。 */
    int8_t rssi;
    /* 自系统启动以来最后收到该 MAC 广播的毫秒时间。 */
    uint32_t last_seen_ms;
} ble_scanner_device_t;

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
    ble_scanner_device_t devices[BLE_SCANNER_MAX_DEVICES];
} ble_scanner_snapshot_t;

/**
 * @brief 初始化 NimBLE Observer 并开始常驻主动扫描。
 *
 * 必须在 nvs_flash_init() 成功后调用。函数只启动 NimBLE 主机任务，不等待第一
 * 个广播包；初始化成功后扫描服务一直运行，打开或关闭 UI 不会启停扫描。
 */
esp_err_t ble_scanner_init(void);

/**
 * @brief 复制当前 BLE 设备快照。
 *
 * 只返回最近 10 秒内出现过的设备。函数可由任意任务调用，内部临界区保证名称、
 * MAC 和 RSSI 来自同一时刻，不需要调用者再加锁。
 */
esp_err_t ble_scanner_get_snapshot(ble_scanner_snapshot_t *snapshot);

#endif
