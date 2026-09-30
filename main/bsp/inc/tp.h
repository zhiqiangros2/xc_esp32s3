#ifndef BOARD_TP_H
#define BOARD_TP_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** CHSC5432 一帧事件最多包含 5 个触点。 */
#define TP_MAX_TOUCH_POINTS 5U

/** 从 CHSC5432 事件地址 0x2000002C 连续读取的原始事件帧长度。 */
#define TP_EVENT_DATA_SIZE 28U

/** tp_receive_state() 永久阻塞等待消息时使用的 timeout_ms 参数。 */
#define TP_WAIT_FOREVER UINT32_MAX

/** CHSC5432 单个触点的事件码。 */
#define TP_TOUCH_EVENT_PUT_DOWN 0x00U
#define TP_TOUCH_EVENT_PUT_UP 0x04U
#define TP_TOUCH_EVENT_CONTACT 0x08U

/** 单个触点的 320x240 LCD/LVGL 坐标和事件码。 */
typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t event;
} tp_point_t;

/** 一次触摸事件；point_count 为 0 表示当前没有触摸。 */
typedef struct {
    uint8_t point_count;
    tp_point_t points[TP_MAX_TOUCH_POINTS];
} tp_state_t;

/**
 * @brief 初始化外接 2.4 英寸电容触摸屏上的 CHSC5432。
 *
 * 调用前必须先初始化板级 I2C 和 AW9523B。函数通过 AW9523B P1_7
 * （TP_CAM_RESET）复位芯片，然后读取 0x20000080~0x2000008E 配置区，
 * 并校验 IC 型号、原始 X/Y 分辨率和最大触点数。重复调用安全。
 */
esp_err_t tp_init(void);

/**
 * @brief 立即通过 I2C 读取 CHSC5432 的 28 字节原始触摸事件帧。
 *
 * 本函数只从事件地址 0x2000002C 读取数据，不解析触点数量、坐标和事件码。
 * event_data 必须指向至少 TP_EVENT_DATA_SIZE 字节的可写缓冲区，
 * event_data_size 用于说明该缓冲区的实际容量。正常运行时由 GPIO42 中断
 * 任务通过 tp_interrupt_process() 调用，读取成功后再调用驱动内部的
 * parse_touch_event() 解析；解析后的状态通过消息队列发送给 LVGL 任务。
 *
 * @param[out] event_data 保存 28 字节原始事件帧的缓冲区。
 * @param[in] event_data_size event_data 缓冲区容量，不能小于 28 字节。
 * @return ESP_OK 读取成功；参数无效、触摸驱动未初始化或 I2C 通信失败时
 * 返回对应错误码。
 */
esp_err_t tp_read_event_frame(uint8_t *event_data, size_t event_data_size);

/**
 * @brief 从触摸消息队列接收一帧已经解析完成的触摸状态。
 *
 * 本函数不会访问 I2C。GPIO42 中断任务负责读取并解析 CHSC5432，然后把
 * tp_state_t 发送到队列；LVGL 任务调用本函数等待下一帧状态。
 *
 * timeout_ms 为 0 时立即返回；普通非零值表示最多阻塞的毫秒数；传入
 * TP_WAIT_FOREVER 时永久阻塞，直到队列收到一帧触摸状态。等待期间不占用
 * CPU。有限等待超时但没有收到消息时返回 ESP_ERR_TIMEOUT。
 *
 * @param[out] state 用于接收触点数量、坐标和事件码，不能为 NULL。
 * @param[in] timeout_ms 最长等待时间，单位为毫秒。
 * @return ESP_OK 成功收到一帧状态；等待超时返回 ESP_ERR_TIMEOUT；参数无效
 * 返回 ESP_ERR_INVALID_ARG；触摸驱动未初始化时返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t tp_receive_state(tp_state_t *state, uint32_t timeout_ms);

/**
 * @brief 处理一次 CHSC5432 触摸中断。
 *
 * 由 GPIO42 共享中断管理任务调用。函数通过 I2C 读取触摸事件、解析坐标，
 * 再把完整的 tp_state_t 发送到触摸消息队列；本函数不能在 ISR 中调用。
 */
esp_err_t tp_interrupt_process(void);

#endif
