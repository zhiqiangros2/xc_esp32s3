/**
 * @file lvgl_camera_page.c
 * @brief GC0308 实时预览页面。
 *
 * 工作流程：
 *
 *   GC0308 中断完成一帧
 *            |
 *            v
 *   camera_read_rgb565_frame() 阻塞返回
 *            |
 *            v
 *   复制到 PSRAM 双缓冲中的一块
 *            |
 *            v
 *   LVGL 显示该缓冲并发出 DRAW_POST
 *            |
 *            v
 *   预览任务切换到另一块缓冲，继续取帧
 *
 * 页面未打开时，预览任务永久阻塞在任务通知上，不轮询摄像头。摄像头驱动
 * 自己的 VSYNC/GDMA 中断和两块帧缓冲始终保留。
 */
#include "lvgl_camera_page.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lvgl_port.h"

#define CAMERA_PAGE_BUFFER_COUNT 2U

/*
 * 预览任务栈放在 PSRAM，避免 ESP-SR、FreeType 和 LVGL 启动后片内堆不足。
 * ESP-IDF 的任务栈大小单位为字节，任务控制块仍由 FreeRTOS 放在片内 RAM。
 */
#define CAMERA_PAGE_TASK_STACK_SIZE 4096U
/* 低于 LVGL 主任务，保证画面合成和触摸事件优先执行。 */
#define CAMERA_PAGE_TASK_PRIORITY 3U

typedef struct {
    /* 当前页面中的 LVGL 对象；页面关闭后全部清空。 */
    lv_obj_t *page;
    lv_obj_t *return_page;
    lv_obj_t *image;
    lv_obj_t *status_label;

    /* 两块 PSRAM 图像缓冲及其对应的 LVGL 图像描述符。 */
    uint8_t *frame_buffers[CAMERA_PAGE_BUFFER_COUNT];
    lv_image_dsc_t frame_descriptors[CAMERA_PAGE_BUFFER_COUNT];

    /* 任务只创建一次，页面关闭后继续存在并阻塞等待下一次启动通知。 */
    TaskHandle_t task_handle;
    bool preview_enabled;
    bool first_frame_pending;

    /* 指向静态语言文本，不需要单独复制或释放。 */
    const char *error_text;
} camera_page_state_t;

static const char *TAG = "CAMERA_PAGE";
static camera_page_state_t s_camera_page;

/* preview_enabled 和 task_handle 会被 LVGL 任务与预览任务共同访问。 */
static portMUX_TYPE s_camera_page_lock = portMUX_INITIALIZER_UNLOCKED;

static void camera_preview_task(void *argument);

/** 返回当前页面是否允许预览任务继续取帧。 */
static bool preview_is_enabled(void)
{
    taskENTER_CRITICAL(&s_camera_page_lock);
    const bool enabled = s_camera_page.preview_enabled;
    taskEXIT_CRITICAL(&s_camera_page_lock);
    return enabled;
}

/** 启动或停止预览，并唤醒任务重新检查状态。 */
static void set_preview_enabled(bool enabled)
{
    taskENTER_CRITICAL(&s_camera_page_lock);
    s_camera_page.preview_enabled = enabled;
    TaskHandle_t task_handle = s_camera_page.task_handle;
    taskEXIT_CRITICAL(&s_camera_page_lock);

    if (task_handle != NULL) {
        xTaskNotifyGive(task_handle);
    }
}

/** 释放双缓冲，只在首次资源初始化失败时使用。 */
static void free_frame_buffers(void)
{
    for (size_t index = 0; index < CAMERA_PAGE_BUFFER_COUNT; ++index) {
        heap_caps_free(s_camera_page.frame_buffers[index]);
        s_camera_page.frame_buffers[index] = NULL;
    }
}

/**
 * 首次打开页面时创建常驻资源，以后直接复用。
 *
 * 每块缓冲保存 320 x 240 x 2 = 153600 字节 RGB565 图像。摄像头输出高
 * 字节在前，因此描述符使用 RGB565_SWAPPED，让 LVGL 直接解释原始字节流。
 */
static esp_err_t prepare_preview_resources(void)
{
    if (s_camera_page.frame_buffers[0] == NULL ||
        s_camera_page.frame_buffers[1] == NULL) {
        free_frame_buffers();

        for (size_t index = 0; index < CAMERA_PAGE_BUFFER_COUNT; ++index) {
            s_camera_page.frame_buffers[index] = heap_caps_malloc(
                CAMERA_FRAME_BUFFER_SIZE,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (s_camera_page.frame_buffers[index] == NULL) {
                ESP_LOGE(TAG,
                         "Cannot allocate camera buffer %u: need=%u, "
                         "PSRAM free/largest=%u/%u",
                         (unsigned)(index + 1U),
                         (unsigned)CAMERA_FRAME_BUFFER_SIZE,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                         (unsigned)heap_caps_get_largest_free_block(
                             MALLOC_CAP_SPIRAM));
                free_frame_buffers();
                return ESP_ERR_NO_MEM;
            }
        }
    }

    for (size_t index = 0; index < CAMERA_PAGE_BUFFER_COUNT; ++index) {
        s_camera_page.frame_descriptors[index] = (lv_image_dsc_t){
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

    if (s_camera_page.task_handle != NULL) {
        return ESP_OK;
    }

    const BaseType_t task_created = xTaskCreateWithCaps(
        camera_preview_task,
        "camera_preview",
        CAMERA_PAGE_TASK_STACK_SIZE,
        NULL,
        CAMERA_PAGE_TASK_PRIORITY,
        &s_camera_page.task_handle,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_created == pdPASS) {
        return ESP_OK;
    }

    s_camera_page.task_handle = NULL;
    ESP_LOGE(TAG,
             "Cannot create preview task: stack=%u, "
             "internal free/largest=%u/%u, PSRAM free/largest=%u/%u",
             (unsigned)CAMERA_PAGE_TASK_STACK_SIZE,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                               MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                        MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    free_frame_buffers();
    return ESP_ERR_NO_MEM;
}

/** LVGL 完成图像绘制后，允许预览任务复用下一块缓冲。 */
static void camera_image_drawn(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_DRAW_POST &&
        lv_event_get_target(event) == s_camera_page.image) {
        xTaskNotifyGive(s_camera_page.task_handle);
    }
}

/** 统一处理页面进入前台和页面被删除两个生命周期事件。 */
static void camera_page_event(lv_event_t *event)
{
    if (lv_event_get_target(event) != s_camera_page.page) {
        return;
    }

    switch (lv_event_get_code(event)) {
        case LV_EVENT_SCREEN_LOADED:
            ESP_LOGI(TAG, "Preview screen loaded; starting camera task");
            set_preview_enabled(true);
            break;

        case LV_EVENT_DELETE:
            set_preview_enabled(false);
            s_camera_page.page = NULL;
            s_camera_page.return_page = NULL;
            s_camera_page.image = NULL;
            s_camera_page.status_label = NULL;
            s_camera_page.error_text = NULL;
            s_camera_page.first_frame_pending = false;
            break;

        default:
            break;
    }
}

/** 返回测试页并异步删除摄像头页面。 */
static void close_button_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    ESP_LOGI(TAG, "Close button clicked; stopping preview");

    lv_obj_t *closing_page = s_camera_page.page;
    lv_obj_t *return_page = s_camera_page.return_page;

    set_preview_enabled(false);
    s_camera_page.page = NULL;
    s_camera_page.return_page = NULL;
    s_camera_page.image = NULL;
    s_camera_page.status_label = NULL;
    s_camera_page.error_text = NULL;
    s_camera_page.first_frame_pending = false;

    lv_screen_load(return_page);
    lv_obj_delete_async(closing_page);
}

/**
 * 预览任务只按事件推进：
 *
 *  1. 页面未打开时阻塞等待 SCREEN_LOADED 通知；
 *  2. 阻塞等待摄像头中断完成一帧，并复制到当前 PSRAM 缓冲；
 *  3. 把该缓冲提交给 LVGL；
 *  4. 阻塞等待 DRAW_POST，确认 LVGL 已经读完缓冲；
 *  5. 切换缓冲并返回第 2 步。
 *
 * 关闭页面会清除 preview_enabled 并发送通知，使任务回到第 1 步。
 */
static void camera_preview_task(void *argument)
{
    (void)argument;

    size_t buffer_index = 0;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (preview_is_enabled()) {
            const esp_err_t result = camera_read_rgb565_frame(
                s_camera_page.frame_buffers[buffer_index],
                CAMERA_FRAME_BUFFER_SIZE);

            /* 页面可能在等待摄像头帧时关闭，此时不再访问 LVGL 对象。 */
            if (!preview_is_enabled()) {
                break;
            }

            if (result != ESP_OK) {
                ESP_LOGE(TAG,
                         "Cannot receive camera frame: %s",
                         esp_err_to_name(result));

                if (lvgl_port_lock(LVGL_PORT_WAIT_FOREVER)) {
                    if (preview_is_enabled() &&
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
                continue;
            }

            bool frame_submitted = false;
            bool first_frame = false;
            if (lvgl_port_lock(LVGL_PORT_WAIT_FOREVER)) {
                if (preview_is_enabled() && s_camera_page.image != NULL) {
                    first_frame = s_camera_page.first_frame_pending;
                    s_camera_page.first_frame_pending = false;

                    lv_image_set_src(
                        s_camera_page.image,
                        &s_camera_page.frame_descriptors[buffer_index]);
                    lv_obj_set_hidden(s_camera_page.image, false);
                    if (s_camera_page.status_label != NULL) {
                        lv_obj_set_hidden(s_camera_page.status_label, true);
                    }
                    lv_obj_invalidate(s_camera_page.image);
                    frame_submitted = true;
                }
                lvgl_port_unlock();
            }

            if (!frame_submitted) {
                break;
            }
            if (first_frame) {
                ESP_LOGI(TAG, "First camera frame submitted to LVGL");
            }

            /* 等待 DRAW_POST 或页面关闭通知。 */
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (preview_is_enabled()) {
                buffer_index = (buffer_index + 1U) %
                               CAMERA_PAGE_BUFFER_COUNT;
            }
        }
    }
}

lv_obj_t *lvgl_camera_page_create(lv_obj_t *return_page,
                                  lvgl_language_t language,
                                  const lv_font_t *ui_font)
{
    if (return_page == NULL) {
        ESP_LOGE(TAG, "Cannot create preview: return page is NULL");
        return NULL;
    }
    if (s_camera_page.page != NULL) {
        ESP_LOGW(TAG, "Cannot create preview: page is already active");
        return NULL;
    }

    ESP_LOGI(TAG,
             "Creating preview: PSRAM free/largest=%u/%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    if (prepare_preview_resources() != ESP_OK) {
        return NULL;
    }

    const lvgl_language_texts_t *texts = lvgl_language_get_texts(language);
    const lv_font_t *font =
        language == LVGL_LANGUAGE_ZH_CN && ui_font != NULL
            ? ui_font
            : LV_FONT_DEFAULT;

    lv_obj_t *page = lv_obj_create(NULL);
    if (page == NULL) {
        ESP_LOGE(TAG, "Cannot create LVGL camera screen");
        return NULL;
    }

    s_camera_page.page = page;
    s_camera_page.return_page = return_page;
    s_camera_page.error_text = texts->camera_error;
    s_camera_page.first_frame_pending = true;

    lv_obj_set_scrollable(page, false);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x101214), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(page,
                        camera_page_event,
                        LV_EVENT_SCREEN_LOADED,
                        NULL);
    lv_obj_add_event_cb(page,
                        camera_page_event,
                        LV_EVENT_DELETE,
                        NULL);

    /* 首帧到达前隐藏图像，在屏幕中央显示等待文字。 */
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

    /* 关闭按钮最后创建，使它始终显示在摄像头图像上方。 */
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

    ESP_LOGI(TAG,
             "Preview page ready: buffers=%p/%p, task=%p",
             (void *)s_camera_page.frame_buffers[0],
             (void *)s_camera_page.frame_buffers[1],
             (void *)s_camera_page.task_handle);
    return page;
}
