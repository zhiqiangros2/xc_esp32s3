#ifndef BOARD_I2C_H
#define BOARD_I2C_H

#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

/** Handle for a device attached to the board I2C bus. */
typedef i2c_master_dev_handle_t board_i2c_device_handle_t;

/**
 * @brief Initialize the BOX3 I2C0 master bus.
 *
 * The board wiring is SCL=GPIO2 and SDA=GPIO3. Calling this function more
 * than once is safe.
 */
esp_err_t board_i2c_init(void);

/**
 * 释放 I2C0 总线。调用前必须先移除通过 board_i2c_add_device() 添加的全部设备；
 * 如果仍有设备占用总线，函数会返回错误并保留当前总线状态。
 */
esp_err_t board_i2c_deinit(void);

/** Add a 7-bit addressed device to the initialized board I2C bus. */
esp_err_t board_i2c_add_device(uint16_t device_address,
                               uint32_t clock_speed_hz,
                               board_i2c_device_handle_t *device);

/** Remove a device previously added with board_i2c_add_device(). */
esp_err_t board_i2c_remove_device(board_i2c_device_handle_t device);

/** Transmit bytes to an I2C device. */
esp_err_t board_i2c_transmit(board_i2c_device_handle_t device,
                             const uint8_t *data,
                             size_t data_size,
                             int timeout_ms);

/** Transmit bytes and then read a response in one I2C transaction. */
esp_err_t board_i2c_transmit_receive(board_i2c_device_handle_t device,
                                     const uint8_t *write_data,
                                     size_t write_size,
                                     uint8_t *read_data,
                                     size_t read_size,
                                     int timeout_ms);

#endif
