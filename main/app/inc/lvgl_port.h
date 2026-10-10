#ifndef BOARD_LVGL_PORT_H
#define BOARD_LVGL_PORT_H

#include "esp_err.h"

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

#endif
