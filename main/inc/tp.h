#ifndef BOARD_TP_H
#define BOARD_TP_H

#include <stdint.h>

#include "esp_err.h"

/** CHSC5432 一帧事件最多包含 5 个触点。 */
#define TP_MAX_TOUCH_POINTS 5U

/** CHSC5432 单个触点的事件码。 */
#define TP_TOUCH_EVENT_PUT_DOWN 0x00U
#define TP_TOUCH_EVENT_PUT_UP 0x04U
#define TP_TOUCH_EVENT_CONTACT 0x08U

/** 单个触点的原始坐标和事件码。 */
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
 * @brief 读取当前触摸状态并返回 CHSC5432 原始坐标。
 *
 * CHSC5432 每次从事件地址 0x2000002C 返回 28 字节，最多解析 5 个触点。
 */
esp_err_t tp_read(tp_state_t *state);

/**
 * @brief 处理一次 CHSC5432 触摸中断。
 *
 * 由 GPIO42 共享中断管理任务调用。函数读取触摸事件以响应中断，并输出
 * 当前触点坐标；本函数不能在 ISR 中调用。
 */
esp_err_t tp_interrupt_process(void);

#endif
