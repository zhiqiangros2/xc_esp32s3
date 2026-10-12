#ifndef APP_LVGL_WIFI_PAGE_H
#define APP_LVGL_WIFI_PAGE_H

#include "lvgl.h"
#include "lvgl_language.h"

/**
 * @brief 创建 STA + SoftAP 配置页面。
 *
 * 页面用三个按钮选择三组 STA + SoftAP 配对配置，并显示当前 STA/AP 的账号、
 * 密码和连接状态。WiFi、NVS、自动重连及 NAPT 都由 BSP 常驻服务负责；关闭
 * 页面时返回 return_page 并销毁本页面，不会停止网络。
 */
lv_obj_t *lvgl_wifi_page_create(lv_obj_t *return_page,
                                lvgl_language_t language,
                                const lv_font_t *ui_font);

#endif
