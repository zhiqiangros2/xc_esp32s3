#ifndef BOARD_LITTLEFS_H
#define BOARD_LITTLEFS_H

#include "esp_err.h"

/** 挂载内部 Flash 上的 LittleFS 分区。 */
esp_err_t littlefs_init(void);

/**
 * 在 LittleFS 中创建 hello.txt，写入并读回校验，然后重命名为 foo.txt。
 */
esp_err_t littlefs_test(void);

/** 卸载 LittleFS 分区；应用正常运行期间不需要调用。 */
esp_err_t littlefs_deinit(void);

#endif
