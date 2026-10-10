/**
 * @file lvgl_camera_page.c
 * @brief GC0308 中断采集帧的 LVGL 实时预览页面。
 *
 * 数据流如下：
 *
 *   GC0308 DVP
 *      |
 *      v
 *   VSYNC/PCLK/GDMA ISR          camera_init() 后始终工作
 *      |
 *      v
 *   esp32-camera 内部事件队列
 *      |
 *      v
 *   camera_read_rgb565_frame()   预览任务阻塞等待，不轮询寄存器
 *      |
 *      v
 *   两个常驻 PSRAM 缓冲交替      一块显示时，另一块接收下一帧
 *      |
 *      v
 *   LVGL image -> 全屏绘制缓冲 -> SPI DMA -> ST7789
 *
 * 没打开页面时，预览任务永久阻塞在任务通知上，不读取帧也不刷新屏幕；
 * esp32-camera 已安装的底层 VSYNC/GDMA 中断仍保留。两个驱动帧缓冲填满
 * 后，CAMERA_GRAB_WHEN_EMPTY 不会覆盖尚未取走的帧，下一次打开页面会先
 * 取得队列中已有的完整帧。
 */
#include "lvgl_camera_page.h"

#include <stdbool.h>
#include <stdint.h>

#include "camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_port.h"

/* 常驻预览任务栈大小；页面关闭时任务阻塞休眠，ESP-IDF 中单位为字节。 */
#define CAMERA_PAGE_TASK_STACK_SIZE 4096U
/* 预览任务低于 LVGL 主任务，保证画面合成和触摸事件优先得到处理。 */
#define CAMERA_PAGE_TASK_PRIORITY 3U

typedef struct {
    /* 当前摄像头页面及其返回页面；return_page 由调用者拥有。 */
    lv_obj_t *page;
    lv_obj_t *return_page;
    /* 全屏图像控件和首帧到达前显示的状态文字。 */
    lv_obj_t *image;
    lv_obj_t *status_label;
    /*
     * 摄像头输出高字节在前，使用 RGB565_SWAPPED 描述符让 LVGL 正确解释
     * 字节顺序，不在每帧中额外交换 76800 个像素。
     */
    lv_image_dsc_t image_descriptors[2];
    /*
     * 双缓冲均位于 PSRAM，每块保存 320x240x2=153600 字节。首次打开页面
     * 时分配，后续页面重复使用，避免任务退出与再次点击之间出现空窗期。
     */
    uint8_t *frame_buffers[2];
    /* 页面加载后才置 true；关闭或删除页面时立即清零。 */
    bool preview_active;
    /* 后台任务句柄同时用于首屏启动、绘制完成和关闭唤醒。 */
    TaskHandle_t task_handle;
    /* 取帧失败时显示的静态语言表字符串。 */
    const char *error_text;
} camera_page_state_t;

static camera_page_state_t s_camera_page;
/* preview_active/task_handle 会被 LVGL 任务和预览任务同时访问。 */
static portMUX_TYPE s_camera_page_lock = portMUX_INITIALIZER_UNLOCKED;
static const char *TAG = "CAMERA_PAGE";

/** 读取预览开关；临界区只保护一个布尔值，不执行任何耗时操作。 */
static bool preview_is_active(void)
{
    taskENTER_CRITICAL(&s_camera_page_lock);
    const bool active = s_camera_page.preview_active;
    taskEXIT_CRITICAL(&s_camera_page_lock);
    return active;
}

/**
 * 修改预览状态并唤醒后台任务。关闭时即使任务正等待图像绘制完成，也能立刻
 * 离开等待；任务若正在 esp_camera_fb_get() 中，会在当前完整帧到达后停止
 * 提交画面并重新进入休眠，但任务本身不删除。
 */
static void set_preview_active(bool active)
{
    taskENTER_CRITICAL(&s_camera_page_lock);
    s_camera_page.preview_active = active;
    TaskHandle_t task_handle = s_camera_page.task_handle;
    taskEXIT_CRITICAL(&s_camera_page_lock);

    if (task_handle != NULL) {
        xTaskNotifyGive(task_handle);
    }
}

/** LVGL 完成图像对象绘制后，允许预览任务复用另一块 PSRAM 缓冲。 */
static void camera_image_drawn(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DRAW_POST ||
        lv_event_get_target(event) != s_camera_page.image) {
        return;
    }

    taskENTER_CRITICAL(&s_camera_page_lock);
    TaskHandle_t task_handle = s_camera_page.task_handle;
    taskEXIT_CRITICAL(&s_camera_page_lock);
    if (task_handle != NULL) {
        xTaskNotifyGive(task_handle);
    }
}

/** 页面真正切换到前台后再启动取帧，避免在隐藏屏幕上等待 DRAW_POST。 */
static void camera_page_loaded(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_SCREEN_LOADED &&
        lv_event_get_target(event) == s_camera_page.page) {
        set_preview_active(true);
    }
}

/**
 * 页面对象被外部删除时断开 LVGL 指针并让常驻任务休眠。关闭按钮会提前把
 * 全局页面指针切走，因此旧页面的延迟删除事件不能清空新页面的指针或状态。
 */
static void camera_page_deleted(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DELETE ||
        lv_event_get_target(event) != s_camera_page.page) {
        return;
    }

    set_preview_active(false);
    s_camera_page.page = NULL;
    s_camera_page.return_page = NULL;
    s_camera_page.image = NULL;
    s_camera_page.status_label = NULL;
}

/** 返回测试页并异步销毁摄像头页面；摄像头硬件和底层中断保持运行。 */
static void close_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    lv_obj_t *closing_page = s_camera_page.page;
    lv_obj_t *return_page = s_camera_page.return_page;

    set_preview_active(false);

    /*
     * 在异步删除旧页面前先清空全局页面指针。用户回到测试页后即使马上再次
     * 点击，也可以直接创建新页面；旧页面稍后的 DELETE 事件会因目标不匹配
     * 而忽略，不会误清理新页面。
     */
    s_camera_page.page = NULL;
    s_camera_page.return_page = NULL;
    s_camera_page.image = NULL;
    s_camera_page.status_label = NULL;

    lv_screen_load(return_page);
    lv_obj_delete_async(closing_page);
}

/**
 * @brief 阻塞接收中断完成帧，并把双缓冲中的新图像提交给 LVGL。
 *
 * 每轮流程：
 *  1. camera_read_rgb565_frame() 睡眠等待驱动帧队列；
 *  2. 中断完成帧到达后，复制到当前未显示的 PSRAM 缓冲；
 *  3. 取得 LVGL 锁，切换 image 描述符并使对象失效；
 *  4. 再次阻塞，直到 LV_EVENT_DRAW_POST 表示 LVGL 已读完该缓冲；
 *  5. 交换两个缓冲的角色并处理下一帧。
 *
 * 因此既没有固定周期轮询，也不会在 LVGL 尚未读完时覆盖正在显示的缓冲。
 */
static void camera_preview_task(void *argument)
{
    (void)argument;

    uint32_t buffer_index = 0;

    while (true) {
        /*
         * 没有摄像头页面时永久休眠。SCREEN_LOADED、DRAW_POST 和关闭事件都
         * 使用同一个任务通知；循环重新检查 preview_active 区分它们的含义。
         */
        while (!preview_is_active()) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }

        while (preview_is_active()) {
            const esp_err_t result = camera_read_rgb565_frame(
                s_camera_page.frame_buffers[buffer_index],
                CAMERA_FRAME_BUFFER_SIZE);

            /* 关闭可能发生在阻塞取帧期间；不再访问已经离开前台的页面。 */
            if (!preview_is_active()) {
                break;
            }

            if (result != ESP_OK) {
                ESP_LOGE(TAG,
                         "Cannot receive camera frame: %s",
                         esp_err_to_name(result));
                if (lvgl_port_lock(LVGL_PORT_WAIT_FOREVER)) {
                    if (preview_is_active() &&
                        s_camera_page.status_label != NULL) {
                        lv_label_set_text(s_camera_page.status_label,
                                          s_camera_page.error_text);
                        lv_obj_set_hidden(s_camera_page.status_label, false);
                        if (s_camera_page.image != NULL) {
                            lv_obj_set_hidden(s_camera_page.image, true);
                        }
                    }
                    lvgl_port_unlock();
                }

                /* 下一轮仍阻塞等待驱动帧，可在摄像头恢复后自动继续预览。 */
                continue;
            }

            bool frame_submitted = false;
            if (lvgl_port_lock(LVGL_PORT_WAIT_FOREVER)) {
                if (preview_is_active() && s_camera_page.image != NULL) {
                    lv_image_set_src(
                        s_camera_page.image,
                        &s_camera_page.image_descriptors[buffer_index]);
                    lv_obj_set_hidden(s_camera_page.image, false);
                    lv_obj_set_hidden(s_camera_page.status_label, true);
                    lv_obj_invalidate(s_camera_page.image);
                    frame_submitted = true;
                }
                lvgl_port_unlock();
            }

            if (!frame_submitted) {
                break;
            }

            /*
             * 不按固定毫秒数猜测 LVGL 是否绘制完成。图像的 DRAW_POST 或
             * 关闭页面都会通知任务，醒来后重新检查页面状态。
             */
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (!preview_is_active()) {
                break;
            }
            buffer_index ^= 1U;
        }
    }
}

lv_obj_t *lvgl_camera_page_create(lv_obj_t *return_page,
                                  lvgl_language_t language,
                                  const lv_font_t *ui_font)
{
    if (return_page == NULL || s_camera_page.page != NULL) {
        return NULL;
    }

    const lvgl_language_texts_t *texts = lvgl_language_get_texts(language);
    const lv_font_t *font = LV_FONT_DEFAULT;
    if (language == LVGL_LANGUAGE_ZH_CN && ui_font != NULL) {
        font = ui_font;
    }

    /*
     * 两块 153600 字节缓冲都明确放入 PSRAM。摄像头驱动自己的两块帧缓冲
     * 仍由 esp32-camera 管理；这里保存的是归还驱动后仍可供 LVGL 使用的副本。
     */
    if (s_camera_page.frame_buffers[0] == NULL) {
        for (size_t index = 0; index < 2; ++index) {
            s_camera_page.frame_buffers[index] = heap_caps_malloc(
                CAMERA_FRAME_BUFFER_SIZE,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (s_camera_page.frame_buffers[index] == NULL) {
                for (size_t release_index = 0;
                     release_index <= index;
                     ++release_index) {
                    heap_caps_free(
                        s_camera_page.frame_buffers[release_index]);
                    s_camera_page.frame_buffers[release_index] = NULL;
                }
                return NULL;
            }
        }
    }

    for (size_t index = 0; index < 2; ++index) {
        s_camera_page.image_descriptors[index] = (lv_image_dsc_t){
            .header = {
                .magic = LV_IMAGE_HEADER_MAGIC,
                .cf = LV_COLOR_FORMAT_RGB565_SWAPPED,
                .w = CAMERA_FRAME_WIDTH,
                .h = CAMERA_FRAME_HEIGHT,
                .stride = CAMERA_FRAME_WIDTH *
                          CAMERA_RGB565_BYTES_PER_PIXEL,
            },
            .data_size = CAMERA_FRAME_BUFFER_SIZE,
            .data = s_camera_page.frame_buffers[index],
        };
    }

    lv_obj_t *page = lv_obj_create(NULL);
    if (page == NULL) {
        return NULL;
    }

    s_camera_page.page = page;
    s_camera_page.return_page = return_page;
    s_camera_page.error_text = texts->camera_error;
    s_camera_page.preview_active = false;

    lv_obj_set_scrollable(page, false);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x101214), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(page,
                        camera_page_loaded,
                        LV_EVENT_SCREEN_LOADED,
                        NULL);
    lv_obj_add_event_cb(page,
                        camera_page_deleted,
                        LV_EVENT_DELETE,
                        NULL);

    /* 首帧前隐藏图像，只在屏幕中央显示等待状态。 */
    s_camera_page.image = lv_image_create(page);
    lv_obj_set_size(s_camera_page.image,
                    CAMERA_FRAME_WIDTH,
                    CAMERA_FRAME_HEIGHT);
    lv_obj_set_pos(s_camera_page.image, 0, 0);
    lv_obj_set_hidden(s_camera_page.image, true);
    lv_obj_add_event_cb(s_camera_page.image,
                        camera_image_drawn,
                        LV_EVENT_DRAW_POST,
                        NULL);

    s_camera_page.status_label = lv_label_create(page);
    lv_label_set_text(s_camera_page.status_label, texts->camera_waiting);
    lv_obj_set_style_text_font(s_camera_page.status_label, font, 0);
    lv_obj_set_style_text_color(s_camera_page.status_label,
                                lv_color_hex(0xFFFFFF),
                                0);
    lv_obj_center(s_camera_page.status_label);

    /* 关闭按钮覆盖在画面右下角，创建顺序保证它始终位于摄像头图像上方。 */
    lv_obj_t *close_button = lv_button_create(page);
    lv_obj_set_size(close_button, 76, 40);
    lv_obj_align(close_button, LV_ALIGN_BOTTOM_RIGHT, -6, -6);
    lv_obj_set_style_radius(close_button, 4, 0);
    lv_obj_set_style_bg_color(close_button, lv_color_hex(0x263238), 0);
    lv_obj_set_style_bg_opa(close_button, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(close_button,
                              lv_color_hex(0x11181C),
                              LV_STATE_PRESSED);
    lv_obj_add_event_cb(close_button,
                        close_button_clicked,
                        LV_EVENT_CLICKED,
                        NULL);

    lv_obj_t *close_label = lv_label_create(close_button);
    lv_label_set_text(close_label, texts->close);
    lv_obj_set_style_text_font(close_label, font, 0);
    lv_obj_set_style_text_color(close_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(close_label);

    /*
     * 任务创建后先永久阻塞，直到页面收到 SCREEN_LOADED。这样页面创建函数
     * 返回到测试页回调、执行 lv_screen_load() 前不会提前提交隐藏图像。
     */
    if (s_camera_page.task_handle == NULL) {
        const BaseType_t task_created = xTaskCreate(
            camera_preview_task,
            "camera_preview",
            CAMERA_PAGE_TASK_STACK_SIZE,
            NULL,
            CAMERA_PAGE_TASK_PRIORITY,
            &s_camera_page.task_handle);
        if (task_created != pdPASS) {
            s_camera_page.task_handle = NULL;
            lv_obj_delete(page);
            for (size_t index = 0; index < 2; ++index) {
                heap_caps_free(s_camera_page.frame_buffers[index]);
                s_camera_page.frame_buffers[index] = NULL;
            }
            return NULL;
        }
    }

    return page;
}
