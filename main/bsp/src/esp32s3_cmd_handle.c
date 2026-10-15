#include "esp32s3_cmd_handle.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ble_client.h"
#include "bsp_info.h"
#include "esp_err.h"
#include "esp_log.h"
#include "speech_recognition.h"
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

#define ESP32S3_SPEECH_START_CMD 0x03U
#define ESP32S3_SPEECH_START_REQUEST_SIZE 1U
#define ESP32S3_SPEECH_START_REQUEST_VALUE 0x00U
#define ESP32S3_SPEECH_START_RESPONSE_SIZE 8U
#define ESP32S3_SPEECH_START_SUCCESS 0xFFU

#define ESP32S3_SPEECH_RESULT_CMD 0x04U
#define ESP32S3_SPEECH_RESULT_REQUEST_SIZE 1U
#define ESP32S3_SPEECH_RESULT_REQUEST_VALUE 0x00U
#define ESP32S3_SPEECH_RESULT_RESPONSE_SIZE 8U
#define ESP32S3_SPEECH_RESULT_SUCCESS 0xFFU

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

/**
 * CMD 0x03 开始新一轮语音识别结果统计。
 *
 * 请求只接受 Data0=0x00。应答固定为 8 字节：Data0 表示是否成功清除上一轮
 * 结果并开始新一轮，Data1~Data7 保持 0。
 */
static void esp32s3_cmd_handle_speech_start(
    const esp32s3_protocol_frame_t *frame)
{
    uint8_t response[ESP32S3_SPEECH_START_RESPONSE_SIZE] = {0};

    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (frame->payload_length == ESP32S3_SPEECH_START_REQUEST_SIZE &&
        frame->payload[0] == ESP32S3_SPEECH_START_REQUEST_VALUE) {
        result = speech_recognition_start();
        if (result == ESP_OK) {
            response[0] = ESP32S3_SPEECH_START_SUCCESS;
        }
    }

    if (result != ESP_OK) {
        ESP_LOGW(TAG,
                 "Cannot start speech recognition round: %s",
                 esp_err_to_name(result));
    }

    const esp_err_t send_result = usb_uart_send(ESP32S3_SPEECH_START_CMD,
                                                 response,
                                                 sizeof(response));
    if (send_result != ESP_OK) {
        ESP_LOGW(TAG,
                 "Failed to send speech start response: %s",
                 esp_err_to_name(send_result));
    }
}

/**
 * CMD 0x04 获取“打开红灯”的语音识别结果。
 *
 * 请求只接受 Data0=0x00。应答固定为 8 字节：识别到“打开红灯”时
 * Data0=0xFF，否则 Data0=0x00，Data1~Data7 保持 0。读取结果不会清除
 * 标志位，只有 CMD 0x03 会清除上一轮结果。
 */
static void esp32s3_cmd_handle_speech_result(
    const esp32s3_protocol_frame_t *frame)
{
    uint8_t response[ESP32S3_SPEECH_RESULT_RESPONSE_SIZE] = {0};

    if (frame->payload_length != ESP32S3_SPEECH_RESULT_REQUEST_SIZE ||
        frame->payload[0] != ESP32S3_SPEECH_RESULT_REQUEST_VALUE) {
        ESP_LOGW(TAG,
                 "Invalid speech result request: payload_len=%u, data0=0x%02X",
                 (unsigned int)frame->payload_length,
                 frame->payload_length > 0U ? frame->payload[0] : 0U);
    } else if (speech_recognition_red_on_detected()) {
        response[0] = ESP32S3_SPEECH_RESULT_SUCCESS;
        ESP_LOGI(TAG, "Speech recognition result: red-on detected");
    } else {
        ESP_LOGI(TAG, "Speech recognition result: red-on not detected");
    }

    const esp_err_t send_result = usb_uart_send(ESP32S3_SPEECH_RESULT_CMD,
                                                 response,
                                                 sizeof(response));
    if (send_result != ESP_OK) {
        ESP_LOGW(TAG,
                 "Failed to send speech result response: %s",
                 esp_err_to_name(send_result));
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

    case ESP32S3_SPEECH_START_CMD:
        esp32s3_cmd_handle_speech_start(frame);
        break;

    case ESP32S3_SPEECH_RESULT_CMD:
        esp32s3_cmd_handle_speech_result(frame);
        break;

    default:
        break;
    }
}
