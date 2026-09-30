#ifndef BOARD_FATFS_H
#define BOARD_FATFS_H

#include "esp_err.h"

/** 挂载内部 Flash 中标签为 vfs 的 FATFS 分区。 */
esp_err_t fatfs_init(void);

/** 在内部 Flash FATFS 上执行容量、写入、读回和重命名测试。 */
esp_err_t fatfs_test(void);

/** 卸载内部 Flash FATFS 分区；正常运行期间不需要调用。 */
esp_err_t fatfs_deinit(void);

#endif
