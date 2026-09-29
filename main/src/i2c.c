#include "i2c.h"

#include <stdbool.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define BOARD_I2C_PORT I2C_NUM_0
#define BOARD_I2C_SCL_GPIO GPIO_NUM_2
#define BOARD_I2C_SDA_GPIO GPIO_NUM_3

struct board_i2c_device {
    i2c_master_dev_handle_t handle;
};

static const char *TAG = "I2C";
static i2c_master_bus_handle_t bus_handle = NULL;
static SemaphoreHandle_t i2c_bus_mutex = NULL;
static bool initialized = false;

esp_err_t board_i2c_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    esp_err_t result = i2c_master_get_bus_handle(BOARD_I2C_PORT, &bus_handle);
    if (result == ESP_ERR_INVALID_STATE) {
        const i2c_master_bus_config_t bus_config = {
            .i2c_port = BOARD_I2C_PORT,
            .sda_io_num = BOARD_I2C_SDA_GPIO,
            .scl_io_num = BOARD_I2C_SCL_GPIO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags.enable_internal_pullup = true,
        };

        result = i2c_new_master_bus(&bus_config, &bus_handle);
        if (result != ESP_OK) {
            bus_handle = NULL;
            return result;
        }
    } else if (result != ESP_OK) {
        bus_handle = NULL;
        return result;
    }

    i2c_bus_mutex = xSemaphoreCreateMutex();
    if (i2c_bus_mutex == NULL) {
        i2c_del_master_bus(bus_handle);
        bus_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    initialized = true;
    ESP_LOGI(TAG, "I2C0 ready: SCL=GPIO2, SDA=GPIO3");
    return ESP_OK;
}

esp_err_t board_i2c_add_device(uint16_t device_address,
                               uint32_t clock_speed_hz,
                               board_i2c_device_handle_t *device)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (device == NULL || device_address > 0x7F || clock_speed_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *device = NULL;
    board_i2c_device_handle_t new_device = calloc(1, sizeof(*new_device));
    if (new_device == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = device_address,
        .scl_speed_hz = clock_speed_hz,
        .scl_wait_us = 0,
        .flags.disable_ack_check = false,
    };

    esp_err_t result = i2c_master_bus_add_device(bus_handle,
                                                  &device_config,
                                                  &new_device->handle);
    if (result != ESP_OK) {
        free(new_device);
        return result;
    }

    *device = new_device;
    return ESP_OK;
}

esp_err_t board_i2c_remove_device(board_i2c_device_handle_t device)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = i2c_master_bus_rm_device(device->handle);
    if (result == ESP_OK) {
        free(device);
    }
    return result;
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
    esp_err_t result = i2c_master_transmit(device->handle,
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
    esp_err_t result = i2c_master_transmit_receive(device->handle,
                                                    write_data,
                                                    write_size,
                                                    read_data,
                                                    read_size,
                                                    timeout_ms);
    xSemaphoreGive(i2c_bus_mutex);
    return result;
}
