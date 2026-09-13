#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SERIAL_FRAME_VERSION 1u
#define SERIAL_FRAME_TYPE_INPUT 0x01u
#define SERIAL_FRAME_TYPE_STATUS 0x81u
#define SERIAL_FRAME_MAX_PAYLOAD 20u
#define SERIAL_FRAME_MAX_DECODED_SIZE (3u + SERIAL_FRAME_MAX_PAYLOAD + 2u)
#define SERIAL_FRAME_MAX_ENCODED_SIZE (SERIAL_FRAME_MAX_DECODED_SIZE + 2u)

typedef void (*serial_frame_cb_t)(uint8_t type,
                                  const uint8_t *payload,
                                  size_t payload_len,
                                  void *context);

typedef struct {
    uint8_t encoded[SERIAL_FRAME_MAX_ENCODED_SIZE];
    size_t encoded_len;
    serial_frame_cb_t callback;
    void *context;
} serial_parser_t;

void serial_parser_init(serial_parser_t *parser,
                        serial_frame_cb_t callback,
                        void *context);
void serial_parser_feed(serial_parser_t *parser,
                        const uint8_t *data,
                        size_t len);
uint16_t serial_crc16_ccitt_false(const uint8_t *data, size_t len);
size_t serial_frame_encode(uint8_t type,
                           const uint8_t *payload,
                           size_t payload_len,
                           uint8_t *out,
                           size_t out_capacity);

#ifdef __cplusplus
}
#endif
