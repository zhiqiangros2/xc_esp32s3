#ifndef BOARD_SPI_H
#define BOARD_SPI_H

#include <stddef.h>

#include "driver/spi_master.h"
#include "esp_err.h"

/**
 * @brief Initialize the BOX3 SPI2 master bus.
 *
 * The board wiring is SCLK=GPIO15, MOSI=GPIO16 and MISO=GPIO17. The first
 * caller specifies the largest DMA transaction required on the bus.
 */
esp_err_t board_spi_init(size_t max_transfer_size);

/** Return the ESP-IDF host identifier for the initialized board SPI bus. */
spi_host_device_t board_spi_get_host(void);

/** Allocate internal DMA-capable memory for a board SPI transaction. */
void *board_spi_dma_alloc(size_t size);

/** Release memory returned by board_spi_dma_alloc(). */
void board_spi_dma_free(void *memory);

/** Release SPI2 when it was initialized by board_spi_init(). */
esp_err_t board_spi_deinit(void);

#endif
