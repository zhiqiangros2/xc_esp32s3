#include "esp32s3_cmd_handle.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ble_client.h"
#include "bsp_info.h"
#include "esp_err.h"
#include "esp_log.h"
#include "usb_uart.h"

#define ESP32S3_VERSION_CMD 0x01U
#define ESP32S3_VERSION_REQUEST_SIZE 1U
#define ESP32S3_VERSION_RESPONSE_SIZE 16U

#define ESP32S3_BLUETOOTH_CMD 0x02U
#define ESP32S3_BLUETOOTH_NAME_FIELD_SIZE 20U
#define ESP32S3_BLUETOOTH_RESERVED_SIZE 4U
#define ESP32S3_BLUETOOTH_REQUEST_SIZE \
    (ESP32S3_BLUETOOTH_NAME_FIELD_SIZE + ESP32S3_BLUETOOTH_RESERVED_SIZE)
#define ESP32S3_BLUETOOTH_RESPONSE_SIZE 16U
#define ESP32S3_BLUETOOTH_SUCCESS 0xFFU

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

static void esp32s3_cmd_send_bluetooth_response(
    const ble_client_query_result_t *result,
    void *context)
{
    (void)context;
    uint8_t response[ESP32S3_BLUETOOTH_RESPONSE_SIZE] = {0};
    if (result != NULL && result->scan_succeeded) {
        response[0] = ESP32S3_BLUETOOTH_SUCCESS;
        memcpy(&response[1], result->address, sizeof(result->address));

        int rssi_magnitude = -(int)result->rssi;
        if (result->rssi == 127 || rssi_magnitude < 0) {
            rssi_magnitude = 0;
        } else if (rssi_magnitude > 100) {
            rssi_magnitude = 100;
        }
        response[7] = (uint8_t)rssi_magnitude;
        if (result->communication_succeeded) {
            response[8] = ESP32S3_BLUETOOTH_SUCCESS;
        }
    }

    const esp_err_t err = usb_uart_send(ESP32S3_BLUETOOTH_CMD,
                                        response,
                                        sizeof(response));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send Bluetooth response: %s",
                 esp_err_to_name(err));
    }
}

static void esp32s3_cmd_handle_bluetooth(
    const esp32s3_protocol_frame_t *frame)
{
    if (frame->payload_length != ESP32S3_BLUETOOTH_REQUEST_SIZE) {
        return;
    }

    size_t name_length = 0U;
    while (name_length < ESP32S3_BLUETOOTH_NAME_FIELD_SIZE &&
           frame->payload[name_length] != '\0') {
        ++name_length;
    }
    if (name_length == 0U) {
        esp32s3_cmd_send_bluetooth_response(NULL, NULL);
        return;
    }

    char target_name[BLE_CLIENT_TARGET_NAME_MAX_LENGTH + 1U] = {0};
    memcpy(target_name, frame->payload, name_length);

    const esp_err_t err = ble_client_query_device(
        target_name,
        esp32s3_cmd_send_bluetooth_response,
        NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Cannot start Bluetooth query for %s: %s",
                 target_name,
                 esp_err_to_name(err));
        esp32s3_cmd_send_bluetooth_response(NULL, NULL);
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

    case ESP32S3_BLUETOOTH_CMD:
        esp32s3_cmd_handle_bluetooth(frame);
        break;

    default:
        break;
    }
}
