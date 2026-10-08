#ifndef BOARD_LVGL_LANGUAGE_H
#define BOARD_LVGL_LANGUAGE_H

#include "esp_err.h"

/** 界面支持的语言。 */
typedef enum {
    LVGL_LANGUAGE_EN,
    LVGL_LANGUAGE_ZH_CN,
    LVGL_LANGUAGE_COUNT,
} lvgl_language_t;

/** 一种语言对应的全部界面文本。 */
typedef struct {
    const char *test_item;
    const char *back;
} lvgl_language_texts_t;

/** 获取指定语言的全部 UTF-8 界面文本，无效语言回退为英文。 */
const lvgl_language_texts_t *lvgl_language_get_texts(lvgl_language_t language);

/** 保存当前界面语言。 */
esp_err_t lvgl_language_set(lvgl_language_t language);

/** 获取当前界面语言。 */
lvgl_language_t lvgl_language_get(void);

#endif
