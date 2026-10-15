#ifndef ESP32S3_PROTOCOL_H
#define ESP32S3_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESP32S3_PROTOCOL_MAX_PAYLOAD_SIZE 254U
#define ESP32S3_PROTOCOL_MAX_FRAME_SIZE \
    (ESP32S3_PROTOCOL_MAX_PAYLOAD_SIZE + 5U)

typedef enum {
    ESP32S3_PROTOCOL_UNPACK_INCOMPLETE,
    ESP32S3_PROTOCOL_UNPACK_COMPLETE,
    ESP32S3_PROTOCOL_UNPACK_INVALID,
} esp32s3_protocol_unpack_status_t;

typedef struct {
    uint8_t command;
    const uint8_t *payload;
    size_t payload_length;
} esp32s3_protocol_frame_t;

/* Frame format: AA 55 LEN CMD PAYLOAD CHECKSUM. LEN includes CMD. */
esp32s3_protocol_unpack_status_t esp32s3_protocol_unpack(
    const uint8_t *data,
    size_t data_length,
    esp32s3_protocol_frame_t *frame,
    size_t *consumed_length);

size_t esp32s3_protocol_build_frame(uint8_t command,
                                    const uint8_t *payload,
                                    size_t payload_length,
                                    uint8_t *frame,
                                    size_t frame_size);

#ifdef __cplusplus
}
#endif

#endif
