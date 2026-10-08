#ifndef BOARD_SPI_H
#define BOARD_SPI_H

#include <stddef.h>

#include "driver/spi_master.h"
#include "esp_err.h"

#define BOARD_SPI_MAX_TRANSFER_SIZE (16U * 1024U) /* SPI2 单次 DMA 传输上限。 */

/**
 * @brief 初始化 BOX3 板的 SPI2 主机总线。
 *
 * 板上 SPI2 引脚为 SCLK=GPIO15、MOSI=GPIO16、MISO=GPIO17。LCD 和 SD
 * 共用这些信号线，但分别使用独立的片选引脚。应用程序应在 main.c 中先调用
 * 本函数，再初始化 LCD 或 SD；重复调用不会重复创建总线。
 * max_transfer_size 必须不超过 BOARD_SPI_MAX_TRANSFER_SIZE。
 */
esp_err_t board_spi_init(size_t max_transfer_size);

/** 返回已经初始化的 SPI2 主机编号，供 LCD 和 SD 配置设备时使用。 */
spi_host_device_t board_spi_get_host(void);

/** 分配可用于 SPI2 DMA 传输的片内内存。 */
void *board_spi_dma_alloc(size_t size);

/** 分配可由 SPI2 直接 DMA 读取的 PSRAM 内存。 */
void *board_spi_psram_dma_alloc(size_t size);

/** 释放 board_spi_dma_alloc() 或 board_spi_psram_dma_alloc() 分配的内存。 */
void board_spi_dma_free(void *memory);

/** 应用程序不再使用 LCD 和 SD 后，释放 SPI2 总线。 */
esp_err_t board_spi_deinit(void);

#endif
