#ifndef ESP32S3_CMD_HANDLE_H
#define ESP32S3_CMD_HANDLE_H

#include "esp32s3_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

void esp32s3_cmd_handle_frame(const esp32s3_protocol_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif
