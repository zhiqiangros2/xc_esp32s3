#include "i2c.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define BOARD_I2C_PORT I2C_NUM_0
#define BOARD_I2C_SCL_GPIO GPIO_NUM_2
#define BOARD_I2C_SDA_GPIO GPIO_NUM_3

static const char *TAG = "I2C";
static i2c_master_bus_handle_t i2c_bus_handle = NULL;
static SemaphoreHandle_t i2c_bus_mutex = NULL;
static bool i2c_bus_owned = false;
static bool i2c_initialized = false;

esp_err_t board_i2c_init(void)
{
    if (i2c_initialized) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        /* 使用 ESP-IDF 默认的 I2C 外设时钟源。 */
        .clk_source = I2C_CLK_SRC_DEFAULT,
        /* 忽略持续时间不超过 7 个采样周期的 SDA/SCL 毛刺脉冲。 */
        .glitch_ignore_cnt = 7,
        /* 使用默认中断优先级，由 ESP-IDF 自动选择合适的优先级。 */
        .intr_priority = 0,
        /* 不创建异步事务队列，设备访问采用同步方式完成。 */
        .trans_queue_depth = 0,
        /* 启用芯片内部上拉；实际硬件仍建议使用外部 I2C 上拉电阻。 */
        .flags.enable_internal_pullup = true,
    };

    /* 本工程由 board_i2c_init() 统一创建 I2C0，不先调用 getter 探测总线。 */
    esp_err_t result = i2c_new_master_bus(&bus_config, &i2c_bus_handle);
    if (result != ESP_OK) {
        i2c_bus_handle = NULL;
        return result;
    }
    i2c_bus_owned = true;

    i2c_bus_mutex = xSemaphoreCreateMutex();
    if (i2c_bus_mutex == NULL) {
        if (i2c_bus_owned) {
            i2c_del_master_bus(i2c_bus_handle);
        }
        i2c_bus_handle = NULL;
        i2c_bus_owned = false;
        return ESP_ERR_NO_MEM;
    }

    i2c_initialized = true;
    ESP_LOGI(TAG, "I2C0 ready: SCL=GPIO2, SDA=GPIO3");
    return ESP_OK;
}

esp_err_t board_i2c_add_device(uint16_t device_address,
                               uint32_t clock_speed_hz,
                               board_i2c_device_handle_t *device)
{
    if (!i2c_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (device == NULL || device_address > 0x7F || clock_speed_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *device = NULL;
    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = device_address,
        .scl_speed_hz = clock_speed_hz,
        .scl_wait_us = 0,
        .flags.disable_ack_check = false,
    };

    return i2c_master_bus_add_device(i2c_bus_handle,
                                     &device_config,
                                     device);
}

esp_err_t board_i2c_remove_device(board_i2c_device_handle_t device)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return i2c_master_bus_rm_device(device);
}

esp_err_t board_i2c_deinit(void)
{
    if (!i2c_initialized) {
        return ESP_OK;
    }

    /*
     * i2c_del_master_bus() 会检查总线上是否仍有设备。AW9523B 或触摸设备
     * 尚未调用 board_i2c_remove_device() 时，直接返回错误并保留当前状态。
     */
    if (i2c_bus_owned) {
        esp_err_t result = i2c_del_master_bus(i2c_bus_handle);
        if (result != ESP_OK) {
            return result;
        }
    }

    if (i2c_bus_mutex != NULL) {
        vSemaphoreDelete(i2c_bus_mutex);
        i2c_bus_mutex = NULL;
    }
    i2c_bus_handle = NULL;
    i2c_bus_owned = false;
    i2c_initialized = false;
    return ESP_OK;
}

esp_err_t board_i2c_transmit(board_i2c_device_handle_t device,
                             const uint8_t *data,
                             size_t data_size,
                             int timeout_ms)
{
    if (device == NULL || data == NULL || data_size == 0 || timeout_ms < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(i2c_bus_mutex, portMAX_DELAY);
    esp_err_t result = i2c_master_transmit(device,
                                           data,
                                           data_size,
                                           timeout_ms);
    xSemaphoreGive(i2c_bus_mutex);
    return result;
}

esp_err_t board_i2c_transmit_receive(board_i2c_device_handle_t device,
                                     const uint8_t *write_data,
                                     size_t write_size,
                                     uint8_t *read_data,
                                     size_t read_size,
                                     int timeout_ms)
{
    if (device == NULL || write_data == NULL || write_size == 0 ||
        read_data == NULL || read_size == 0 || timeout_ms < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(i2c_bus_mutex, portMAX_DELAY);
    esp_err_t result = i2c_master_transmit_receive(device,
                                                    write_data,
                                                    write_size,
                                                    read_data,
                                                    read_size,
                                                    timeout_ms);
    xSemaphoreGive(i2c_bus_mutex);
    return result;
}
