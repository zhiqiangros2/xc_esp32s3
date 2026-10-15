#include "esp32s3_protocol.h"

#include <string.h>

#define ESP32S3_PROTOCOL_HEADER_0 0xAAU
#define ESP32S3_PROTOCOL_HEADER_1 0x55U

esp32s3_protocol_unpack_status_t esp32s3_protocol_unpack(
    const uint8_t *data,
    size_t data_length,
    esp32s3_protocol_frame_t *frame,
    size_t *consumed_length)
{
    if (frame == NULL || consumed_length == NULL ||
        (data == NULL && data_length != 0U)) {
        return ESP32S3_PROTOCOL_UNPACK_INVALID;
    }

    *consumed_length = 0U;
    frame->command = 0U;
    frame->payload = NULL;
    frame->payload_length = 0U;

    size_t header_index = 0U;
    while (header_index + 1U < data_length) {
        if (data[header_index] == ESP32S3_PROTOCOL_HEADER_0 &&
            data[header_index + 1U] == ESP32S3_PROTOCOL_HEADER_1) {
            break;
        }
        ++header_index;
    }

    if (header_index + 1U >= data_length) {
        if (data_length != 0U &&
            data[data_length - 1U] == ESP32S3_PROTOCOL_HEADER_0) {
            *consumed_length = data_length - 1U;
        } else {
            *consumed_length = data_length;
        }
        return ESP32S3_PROTOCOL_UNPACK_INCOMPLETE;
    }

    if (header_index != 0U) {
        *consumed_length = header_index;
        return ESP32S3_PROTOCOL_UNPACK_INVALID;
    }

    if (data_length < 3U) {
        return ESP32S3_PROTOCOL_UNPACK_INCOMPLETE;
    }

    const uint8_t frame_data_length = data[2];
    if (frame_data_length == 0U) {
        *consumed_length = 1U;
        return ESP32S3_PROTOCOL_UNPACK_INVALID;
    }

    const size_t frame_length = (size_t)frame_data_length + 4U;
    if (data_length < frame_length) {
        return ESP32S3_PROTOCOL_UNPACK_INCOMPLETE;
    }

    uint8_t checksum = 0U;
    for (size_t i = 2U; i < frame_length - 1U; ++i) {
        checksum ^= data[i];
    }
    if (checksum != data[frame_length - 1U]) {
        *consumed_length = 1U;
        return ESP32S3_PROTOCOL_UNPACK_INVALID;
    }

    frame->command = data[3];
    frame->payload = &data[4];
    frame->payload_length = (size_t)frame_data_length - 1U;
    *consumed_length = frame_length;
    return ESP32S3_PROTOCOL_UNPACK_COMPLETE;
}

size_t esp32s3_protocol_build_frame(uint8_t command,
                                    const uint8_t *payload,
                                    size_t payload_length,
                                    uint8_t *frame,
                                    size_t frame_size)
{
    if (payload_length > ESP32S3_PROTOCOL_MAX_PAYLOAD_SIZE ||
        (payload == NULL && payload_length != 0U) ||
        frame == NULL) {
        return 0U;
    }

    const size_t required_size = payload_length + 5U;
    if (frame_size < required_size) {
        return 0U;
    }

    const uint8_t length = (uint8_t)(payload_length + 1U);
    frame[0] = ESP32S3_PROTOCOL_HEADER_0;
    frame[1] = ESP32S3_PROTOCOL_HEADER_1;
    frame[2] = length;
    frame[3] = command;
    if (payload_length != 0U) {
        memcpy(&frame[4], payload, payload_length);
    }

    uint8_t checksum = length ^ command;
    for (size_t i = 0; i < payload_length; ++i) {
        checksum ^= payload[i];
    }
    frame[payload_length + 4U] = checksum;

    return required_size;
}
