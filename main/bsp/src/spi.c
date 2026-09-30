#include "spi.h"

#include <stdbool.h>
#include <limits.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define BOARD_SPI_HOST SPI2_HOST
#define BOARD_SPI_SCLK_GPIO GPIO_NUM_15
#define BOARD_SPI_MOSI_GPIO GPIO_NUM_16
#define BOARD_SPI_MISO_GPIO GPIO_NUM_17
static const char *TAG = "SPI";
static bool spi2_initialized = false;
static bool spi2_bus_owned = false;

esp_err_t board_spi_init(size_t max_transfer_size)
{
    if (max_transfer_size == 0 ||
        max_transfer_size > BOARD_SPI_MAX_TRANSFER_SIZE ||
        max_transfer_size > INT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    /* LCD 和 SD 都使用同一条已经初始化的 SPI2，总线只创建一次。 */
    if (spi2_initialized) {
        return ESP_OK;
    }

    const spi_bus_config_t bus_config = {
        .mosi_io_num = BOARD_SPI_MOSI_GPIO,
        .miso_io_num = BOARD_SPI_MISO_GPIO,
        .sclk_io_num = BOARD_SPI_SCLK_GPIO,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        /*
         * 使用固定的总线级上限，使 LCD 和 SD 无论谁先初始化，都能满足
         * 之后加入总线的另一个设备，不受第一次调用参数大小影响。
         */
        .max_transfer_sz = BOARD_SPI_MAX_TRANSFER_SIZE,
    };

    esp_err_t result = spi_bus_initialize(BOARD_SPI_HOST,
                                           &bus_config,
                                           SPI_DMA_CH_AUTO);
    if (result == ESP_OK) {
        spi2_bus_owned = true;
    } else if (result == ESP_ERR_INVALID_STATE) {
        /* 总线可能已由其他板级模块创建，复用它但不负责释放。 */
        spi2_bus_owned = false;
    } else {
        return result;
    }

    spi2_initialized = true;
    ESP_LOGI(TAG,
             "SPI2 ready: SCLK=%d, MOSI=%d, MISO=%d, max transfer=%u bytes",
             BOARD_SPI_SCLK_GPIO,
             BOARD_SPI_MOSI_GPIO,
             BOARD_SPI_MISO_GPIO,
             (unsigned int)BOARD_SPI_MAX_TRANSFER_SIZE);
    return ESP_OK;
}

spi_host_device_t board_spi_get_host(void)
{
    return BOARD_SPI_HOST;
}

void *board_spi_dma_alloc(size_t size)
{
    if (!spi2_initialized || size == 0) {
        return NULL;
    }

    /* 强制传输缓冲区使用片内 RAM；此函数还会满足 SPI DMA 能力和内存对齐要求。 */
    return spi_bus_dma_memory_alloc(BOARD_SPI_HOST,
                                    size,
                                    MALLOC_CAP_INTERNAL);
}

void board_spi_dma_free(void *memory)
{
    heap_caps_free(memory);
}

esp_err_t board_spi_deinit(void)
{
    if (!spi2_initialized) {
        return ESP_OK;
    }

    if (spi2_bus_owned) {
        esp_err_t result = spi_bus_free(BOARD_SPI_HOST);
        if (result != ESP_OK) {
            return result;
        }
    }

    spi2_initialized = false;
    spi2_bus_owned = false;
    return ESP_OK;
}
