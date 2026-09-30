#ifndef BOARD_LVGL_PORT_H
#define BOARD_LVGL_PORT_H

#include "esp_err.h"

/**
 * @brief 初始化 LVGL、320x240 LCD 刷新接口、中断式触摸输入和处理任务。
 *
 * 调用前必须完成 lcd_init()、tp_init() 和 GPIO42 共享中断管理器初始化。
 * CHSC5432 由中断任务读取并把状态发送到消息队列。独立触摸任务永久阻塞
 * 等待队列，收到消息后获取 LVGL 全局互斥锁并上报；主 LVGL 任务继续负责
 * 定时器和显示刷新。整个过程不会周期轮询 I2C。函数允许重复调用。
 */
esp_err_t lvgl_port_init(void);

/**
 * @brief 获取 LVGL 全局互斥锁。
 *
 * LVGL 不是线程安全的。应用任务创建或修改界面前必须调用本函数，完成后
 * 调用 lvgl_port_unlock()。LVGL 事件回调已经在 LVGL 任务中，不需要加锁。
 */
esp_err_t lvgl_port_lock(void);

/** 释放由 lvgl_port_lock() 获取的 LVGL 全局互斥锁。 */
void lvgl_port_unlock(void);

#endif
