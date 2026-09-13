#include "serial_parser.h"

#include <stdbool.h>
#include <string.h>

static bool serial_type_length_valid(uint8_t type, size_t payload_len)
{
    return (type == SERIAL_FRAME_TYPE_INPUT && payload_len == 17u) ||
           (type == SERIAL_FRAME_TYPE_STATUS && payload_len == 20u);
}

uint16_t serial_crc16_ccitt_false(const uint8_t *data, size_t len)
{
    uint16_t crc = UINT16_C(0xffff);

    if (data == NULL && len != 0u) {
        return crc;
    }

    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned bit = 0; bit < 8u; ++bit) {
            crc = (crc & UINT16_C(0x8000)) != 0u
                      ? (uint16_t)((crc << 1) ^ UINT16_C(0x1021))
                      : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static size_t cobs_encode(const uint8_t *input,
                          size_t input_len,
                          uint8_t *output,
                          size_t output_capacity)
{
    if (output_capacity == 0u) {
        return 0u;
    }

    size_t read_index = 0u;
    size_t write_index = 1u;
    size_t code_index = 0u;
    uint8_t code = 1u;

    while (read_index < input_len) {
        if (input[read_index] == 0u) {
            if (code_index >= output_capacity) {
                return 0u;
            }
            output[code_index] = code;
            code_index = write_index++;
            code = 1u;
            ++read_index;
        } else {
            if (write_index >= output_capacity) {
                return 0u;
            }
            output[write_index++] = input[read_index++];
            ++code;
            if (code == 0xffu) {
                if (code_index >= output_capacity) {
                    return 0u;
                }
                output[code_index] = code;
                code_index = write_index++;
                code = 1u;
            }
        }
    }

    if (code_index >= output_capacity) {
        return 0u;
    }
    output[code_index] = code;
    return write_index;
}

static size_t cobs_decode(const uint8_t *input,
                          size_t input_len,
                          uint8_t *output,
                          size_t output_capacity)
{
    size_t read_index = 0u;
    size_t write_index = 0u;

    while (read_index < input_len) {
        const uint8_t code = input[read_index++];
        if (code == 0u || (size_t)(code - 1u) > input_len - read_index) {
            return 0u;
        }

        for (uint8_t i = 1u; i < code; ++i) {
            if (write_index >= output_capacity) {
                return 0u;
            }
            output[write_index++] = input[read_index++];
        }

        if (code != 0xffu && read_index < input_len) {
            if (write_index >= output_capacity) {
                return 0u;
            }
            output[write_index++] = 0u;
        }
    }

    return write_index;
}

size_t serial_frame_encode(uint8_t type,
                           const uint8_t *payload,
                           size_t payload_len,
                           uint8_t *out,
                           size_t out_capacity)
{
    if (!serial_type_length_valid(type, payload_len) || payload == NULL || out == NULL) {
        return 0u;
    }

    uint8_t decoded[SERIAL_FRAME_MAX_DECODED_SIZE];
    const size_t decoded_len = 3u + payload_len + 2u;
    decoded[0] = SERIAL_FRAME_VERSION;
    decoded[1] = type;
    decoded[2] = (uint8_t)payload_len;
    memcpy(&decoded[3], payload, payload_len);

    const uint16_t crc = serial_crc16_ccitt_false(decoded, 3u + payload_len);
    decoded[3u + payload_len] = (uint8_t)crc;
    decoded[4u + payload_len] = (uint8_t)(crc >> 8);

    if (out_capacity < 2u) {
        return 0u;
    }
    const size_t encoded_len = cobs_encode(decoded, decoded_len, out, out_capacity - 1u);
    if (encoded_len == 0u || encoded_len >= out_capacity) {
        return 0u;
    }
    out[encoded_len] = 0u;
    return encoded_len + 1u;
}

void serial_parser_init(serial_parser_t *parser,
                        serial_frame_cb_t callback,
                        void *context)
{
    if (parser == NULL) {
        return;
    }
    memset(parser, 0, sizeof(*parser));
    parser->callback = callback;
    parser->context = context;
}

static void serial_parser_finish_frame(serial_parser_t *parser)
{
    if (parser->encoded_len == 0u || parser->encoded_len > sizeof(parser->encoded)) {
        parser->encoded_len = 0u;
        return;
    }

    uint8_t decoded[SERIAL_FRAME_MAX_DECODED_SIZE];
    const size_t decoded_len = cobs_decode(parser->encoded,
                                           parser->encoded_len,
                                           decoded,
                                           sizeof(decoded));
    parser->encoded_len = 0u;

    if (decoded_len < 5u || decoded[0] != SERIAL_FRAME_VERSION) {
        return;
    }

    const size_t payload_len = decoded[2];
    if (!serial_type_length_valid(decoded[1], payload_len) ||
        decoded_len != 3u + payload_len + 2u) {
        return;
    }

    const uint16_t expected_crc = (uint16_t)decoded[3u + payload_len] |
                                  ((uint16_t)decoded[4u + payload_len] << 8);
    if (serial_crc16_ccitt_false(decoded, 3u + payload_len) != expected_crc) {
        return;
    }

    if (parser->callback != NULL) {
        parser->callback(decoded[1], &decoded[3], payload_len, parser->context);
    }
}

void serial_parser_feed(serial_parser_t *parser,
                        const uint8_t *data,
                        size_t len)
{
    if (parser == NULL || (data == NULL && len != 0u)) {
        return;
    }

    for (size_t i = 0; i < len; ++i) {
        if (data[i] == 0u) {
            serial_parser_finish_frame(parser);
            continue;
        }

        if (parser->encoded_len < sizeof(parser->encoded)) {
            parser->encoded[parser->encoded_len++] = data[i];
        } else {
            parser->encoded_len = sizeof(parser->encoded) + 1u;
        }
    }
}
