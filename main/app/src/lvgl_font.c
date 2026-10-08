/**
 * @file lvgl_font.c
 * @brief 从 SD 卡加载 FreeType TTF 字体，并将字形缓存放入 PSRAM。
 */

#include "lvgl_font.h"

#include <sys/stat.h>

#include "esp_log.h"

/* 当前界面使用的字体字号，单位为像素。 */
#define UI_FONT_SIZE 18U

/* 字体只创建一次，后续调用直接复用同一个 lv_font_t。 */
static const char *TAG = "LVGL_FONT";
static lv_font_t *ui_font = NULL;

/**
 * 从 SD 卡加载界面 TTF 字体。
 *
 * 字体文件必须位于 LVGL_UI_FONT_PATH，通常为 /sdcard/font/font.ttf。
 * 函数具有幂等性：字体已经加载时直接返回之前创建的字体对象。
 */
esp_err_t lvgl_font_load_ui(lv_font_t **font)
{
    if (font == NULL) {
        /* 调用者必须提供用于接收 lv_font_t 指针的地址。 */
        return ESP_ERR_INVALID_ARG;
    }

    *font = NULL;
    if (ui_font != NULL) {
        /* 避免重复打开和解析 17 MB 级别的 TTF 文件。 */
        *font = ui_font;
        return ESP_OK;
    }

    /* 先检查文件是否存在且大小有效，再交给 FreeType 解析。 */
    struct stat font_status;
    if (stat(LVGL_UI_FONT_PATH, &font_status) != 0 ||
        font_status.st_size <= 0) {
        ESP_LOGE(TAG, "TTF font not found: %s", LVGL_UI_FONT_PATH);
        return ESP_ERR_NOT_FOUND;
    }

    /* 使用位图渲染模式，FreeType 会按需生成字符的灰度位图。 */
    ui_font = lv_freetype_font_create(LVGL_UI_FONT_PATH,
                                      LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                      UI_FONT_SIZE,
                                      LV_FREETYPE_FONT_STYLE_NORMAL);
    if (ui_font == NULL) {
        /* 文件存在但格式不正确、内存不足或 FreeType 初始化失败。 */
        ESP_LOGE(TAG, "Failed to load TTF font: %s", LVGL_UI_FONT_PATH);
        return ESP_FAIL;
    }

    *font = ui_font;
    ESP_LOGI(TAG,
             "Loaded TTF font from %s (%ld bytes)",
             LVGL_UI_FONT_PATH,
             (long)font_status.st_size);
    return ESP_OK;
}
