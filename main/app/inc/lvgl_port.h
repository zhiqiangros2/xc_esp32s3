#ifndef BOARD_LVGL_PORT_H
#define BOARD_LVGL_PORT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** lvgl_port_lock() 永久等待 LVGL 所有权时使用的特殊超时值。 */
#define LVGL_PORT_WAIT_FOREVER UINT32_MAX

/**
 * @brief 初始化 LVGL、320x240 LCD 刷新接口和中断式触摸输入。
 *
 * 调用前必须完成 lcd_init()、tp_init() 和 GPIO42 共享中断管理器初始化。
 * 本函数不会启动刷新任务，应用应先创建完整界面，再调用 lvgl_port_start()，
 * 避免默认空白屏在首个应用界面之前被刷新。函数允许重复调用。
 */
esp_err_t lvgl_port_init(void);

/**
 * @brief 启动统一处理界面刷新和触摸输入的 LVGL 任务。
 *
 * 必须在 lvgl_port_init() 和首屏界面创建完成后调用。任务使用链接期预留的
 * 32 KiB 片内静态栈，确保 FreeType 读取 SD 字体时 SDSPI 的局部控制数据可被
 * SPI DMA 直接访问；两个全屏绘制缓冲仍位于 PSRAM。函数允许重复调用。
 */
esp_err_t lvgl_port_start(void);

/**
 * @brief 取得 LVGL 全局递归互斥锁。
 *
 * LVGL API 默认不是线程安全的。页面事件回调已经运行在 LVGL 主任务中；
 * 摄像头等后台任务若要更新控件，必须先调用本函数，完成全部 LVGL 操作后
 * 再调用 lvgl_port_unlock()。禁止在 ISR 中调用。
 *
 * @param[in] timeout_ms 0 表示不等待，普通值表示最长等待毫秒数，
 * LVGL_PORT_WAIT_FOREVER 表示永久阻塞等待。
 * @return true 已取得锁；false 端口未初始化或等待超时。
 */
bool lvgl_port_lock(uint32_t timeout_ms);

/** 释放当前任务已经取得的 LVGL 全局递归互斥锁。 */
void lvgl_port_unlock(void);

#endif
