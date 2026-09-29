#ifndef BOARD_SD_H
#define BOARD_SD_H

#include <stdint.h>

#include "esp_err.h"

#define SD_MOUNT_POINT "/sdcard"

/**
 * @brief 通过共享的 SPI2 总线挂载 BOX3 的 microSD 卡。
 *
 * SD 卡与 LCD 共用 SCLK=GPIO15、MOSI=GPIO16 和 MISO=GPIO17，SD 卡自身使用
 * GPIO18 作为片选信号。调用本函数前必须先调用 board_spi_init()。
 * 本函数允许重复调用；挂载失败时不会自动格式化 SD 卡。
 */
esp_err_t sd_init(void);

/** 获取 FAT 文件系统的总容量和当前可用字节数。 */
esp_err_t sd_get_usage(uint64_t *total_bytes, uint64_t *free_bytes);

/** 卸载 SD 卡；应用程序拥有的 SPI2 总线仍保持可用。 */
esp_err_t sd_deinit(void);

#endif
