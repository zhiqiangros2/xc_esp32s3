#ifndef BOARD_LVGL_TEST_PAGE_H
#define BOARD_LVGL_TEST_PAGE_H

#include "lvgl.h"
#include "lvgl_language.h"

/**
 * @brief 创建带有返回按钮的测试页面。
 *
 * 页面顶部显示音乐、录音和 WAV 管理按钮，右下角显示返回按钮。子页面按需
 * 创建并在关闭时销毁；返回按钮切换回 main_page，并异步销毁测试页面。
 * 本函数只能在首屏任务启动前或 LVGL 事件回调中调用。
 *
 * @param[in] main_page 返回按钮需要返回的主界面，不能为 NULL。
 * @param[in] language 初始界面语言。
 * @param[in] ui_font 界面字体；使用中文时不能为 NULL。
 * @return 创建成功时返回测试页面；参数无效或创建失败时返回 NULL。
 */
lv_obj_t *lvgl_test_page_create(lv_obj_t *main_page,
                                lvgl_language_t language,
                                const lv_font_t *ui_font);

/** 更新已创建测试页面的语言，调用时必须持有 LVGL 互斥锁。 */
void lvgl_test_page_set_language(lvgl_language_t language,
                                 const lv_font_t *ui_font);

#endif
