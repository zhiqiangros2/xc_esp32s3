#ifndef BOARD_LVGL_MUSIC_PAGE_H
#define BOARD_LVGL_MUSIC_PAGE_H

#include "lvgl.h"
#include "lvgl_language.h"

/**
 * @brief 创建 SD 卡 WAV 音乐页面。
 *
 * 页面创建时扫描 /sdcard/music。点击文件开始后台播放；停止按钮只停止播放，
 * 关闭按钮会先停止播放、返回 return_page，再销毁音乐页面。
 */
lv_obj_t *lvgl_music_page_create(lv_obj_t *return_page,
                                 lvgl_language_t language,
                                 const lv_font_t *ui_font);

#endif
