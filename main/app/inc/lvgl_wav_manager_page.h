#ifndef APP_LVGL_WAV_MANAGER_PAGE_H
#define APP_LVGL_WAV_MANAGER_PAGE_H

#include "lvgl.h"
#include "lvgl_language.h"

/**
 * @brief 创建 SD 卡 WAV 文件管理页面。
 *
 * 页面首帧显示后逐项扫描 /sdcard/music 并添加 WAV 文件按钮，不会因完整目录
 * 扫描而阻塞页面打开。用户先选中文件，再点击删除按钮；删除成功后页面会重新
 * 增量扫描目录并更新列表。关闭页面时返回 return_page，并销毁本页面及所有
 * 文件项上下文。
 */
lv_obj_t *lvgl_wav_manager_page_create(lv_obj_t *return_page,
                                       lvgl_language_t language,
                                       const lv_font_t *ui_font);

#endif
