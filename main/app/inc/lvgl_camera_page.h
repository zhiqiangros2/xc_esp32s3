#ifndef BOARD_LVGL_CAMERA_PAGE_H
#define BOARD_LVGL_CAMERA_PAGE_H

#include "lvgl.h"
#include "lvgl_language.h"

/**
 * @brief 创建 GC0308 实时预览页面。
 *
 * 常驻后台任务通过 camera_read_rgb565_frame() 阻塞等待摄像头中断完成帧，
 * 再把 320x240 RGB565 图像交给 LVGL。关闭时立即返回 return_page 并异步
 * 销毁预览页面；后台任务回到阻塞状态，首次打开时分配的双图像缓冲保留供
 * 下次页面直接复用，避免关闭后任务退出期间再次点击没有响应。
 * camera_init() 建立的摄像头硬件、DMA 和中断也始终保持运行。
 *
 * @param[in] return_page 关闭后返回的已有页面，不能为 NULL。
 * @param[in] language 页面文本语言。
 * @param[in] ui_font 中文界面字体；英文或不需要中文时可以为 NULL。
 * @return 创建成功时返回独立页面；资源不足或已有预览页面时返回 NULL。
 */
lv_obj_t *lvgl_camera_page_create(lv_obj_t *return_page,
                                  lvgl_language_t language,
                                  const lv_font_t *ui_font);

#endif
