#include "lvgl_language.h"

static lvgl_language_t s_language = LVGL_LANGUAGE_ZH_CN;

static const lvgl_language_texts_t s_texts[LVGL_LANGUAGE_COUNT] = {
    [LVGL_LANGUAGE_EN] = {
        .test_item = "Test item",
        .back = "Back",
    },
    [LVGL_LANGUAGE_ZH_CN] = {
        .test_item = "测试项",
        .back = "返回",
    },
};

const lvgl_language_texts_t *lvgl_language_get_texts(lvgl_language_t language)
{
    if (language < 0 || language >= LVGL_LANGUAGE_COUNT) {
        language = LVGL_LANGUAGE_EN;
    }

    return &s_texts[language];
}

esp_err_t lvgl_language_set(lvgl_language_t language)
{
    if (language < 0 || language >= LVGL_LANGUAGE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    s_language = language;
    return ESP_OK;
}

lvgl_language_t lvgl_language_get(void)
{
    return s_language;
}
