#ifndef APP_LVGL_BLUETOOTH_PAGE_H
#define APP_LVGL_BLUETOOTH_PAGE_H

#include "lvgl.h"
#include "lvgl_language.h"

/**
 * @brief 创建 BLE 扫描结果页面。
 *
 * BSP 扫描服务始终运行，本页面只读取设备快照并显示广播名称、MAC 地址和 RSSI。
 * 关闭页面时返回 return_page 并销毁本页面，不会停止 BLE 扫描。
 */
lv_obj_t *lvgl_bluetooth_page_create(lv_obj_t *return_page,
                                     lvgl_language_t language,
                                     const lv_font_t *ui_font);

#endif
