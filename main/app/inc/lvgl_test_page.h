#ifndef BOARD_LVGL_TEST_PAGE_H
#define BOARD_LVGL_TEST_PAGE_H

#include "lvgl.h"

/**
 * @brief 创建带有返回按钮的测试页面。
 *
 * 页面右下角显示 Back 按钮，页面中央显示 New Page。点击 Back 后切换回
 * main_page。调用本函数时必须已经持有 lvgl_port_lock() 对应的互斥锁。
 *
 * @param[in] main_page Back 按钮需要返回的主界面，不能为 NULL。
 * @return 创建成功时返回测试页面；参数无效或创建失败时返回 NULL。
 */
lv_obj_t *lvgl_test_page_create(lv_obj_t *main_page);

#endif
