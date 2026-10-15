#ifndef USB_UART_H
#define USB_UART_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the TinyUSB CDC-ACM command channel on GPIO19 (D-) and GPIO20 (D+). */
esp_err_t usb_uart_init(void);

/** Pack and send one protocol frame through the native USB channel. */
esp_err_t usb_uart_send(uint8_t command,
                        const uint8_t *payload,
                        size_t payload_length);

#ifdef __cplusplus
}
#endif

#endif
