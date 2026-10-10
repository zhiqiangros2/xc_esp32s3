#ifndef APP_LVGL_RECORD_PAGE_H
#define APP_LVGL_RECORD_PAGE_H

#include "lvgl.h"
#include "lvgl_language.h"

/**
 * 创建录音页面。关闭时先停止录音，文件封装完成后返回并销毁本页面。
 */
lv_obj_t *lvgl_record_page_create(lv_obj_t *return_page,
                                  lvgl_language_t language,
                                  const lv_font_t *ui_font);

#endif
