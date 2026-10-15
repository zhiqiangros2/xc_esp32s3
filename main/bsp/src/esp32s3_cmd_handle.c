#include "esp32s3_cmd_handle.h"

#include <stddef.h>
#include <stdint.h>

#include "bsp_info.h"
#include "esp_err.h"
#include "esp_log.h"
#include "usb_uart.h"

#define ESP32S3_VERSION_CMD 0x01U
#define ESP32S3_VERSION_REQUEST_SIZE 1U
#define ESP32S3_VERSION_RESPONSE_SIZE 16U

static const char *TAG = "ESP32S3_CMD";

static void esp32s3_cmd_handle_version(
    const esp32s3_protocol_frame_t *frame)
{
    if (frame->payload_length != ESP32S3_VERSION_REQUEST_SIZE ||
        frame->payload[0] != 0x00U) {
        return;
    }

    uint8_t response[ESP32S3_VERSION_RESPONSE_SIZE] = {0};
    bsp_info_get_software_versions(&response[0], &response[3]);

    esp_err_t err = usb_uart_send(frame->command, response, sizeof(response));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send software version response: %s",
                 esp_err_to_name(err));
    }
}

void esp32s3_cmd_handle_frame(const esp32s3_protocol_frame_t *frame)
{
    if (frame == NULL) {
        return;
    }

    switch (frame->command) {
    case ESP32S3_VERSION_CMD:
        esp32s3_cmd_handle_version(frame);
        break;

    default:
        break;
    }
}
