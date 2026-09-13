#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "serial_parser.h"
#include "unity.h"

static const uint8_t k_input_payload[17] = {
    0x02, 0x01, 0x40, 0x00, 0x00, 0xff, 0x7f, 0x01, 0x80,
    0x00, 0x00, 0x00, 0x40, 0x04, 0x03, 0x02, 0x01,
};

static const uint8_t k_input_frame[24] = {
    0x07, 0x01, 0x01, 0x11, 0x02, 0x01, 0x40, 0x01,
    0x05, 0xff, 0x7f, 0x01, 0x80, 0x01, 0x01, 0x08,
    0x40, 0x04, 0x03, 0x02, 0x01, 0x1d, 0x95, 0x00,
};

typedef struct {
    size_t calls;
    uint8_t types[4];
    size_t lengths[4];
    uint8_t payloads[4][SERIAL_FRAME_MAX_PAYLOAD];
} callback_capture_t;

static void capture_frame(uint8_t type,
                          const uint8_t *payload,
                          size_t payload_len,
                          void *context)
{
    callback_capture_t *capture = context;
    TEST_ASSERT_LESS_THAN(4u, capture->calls);
    const size_t index = capture->calls++;
    capture->types[index] = type;
    capture->lengths[index] = payload_len;
    memcpy(capture->payloads[index], payload, payload_len);
}

static size_t test_cobs_encode(const uint8_t *input,
                               size_t input_len,
                               uint8_t *output,
                               size_t output_capacity)
{
    size_t read_index = 0;
    size_t write_index = 1;
    size_t code_index = 0;
    uint8_t code = 1;

    if (output_capacity == 0) {
        return 0;
    }
    while (read_index < input_len) {
        if (input[read_index] == 0) {
            output[code_index] = code;
            code_index = write_index++;
            code = 1;
            ++read_index;
        } else {
            output[write_index++] = input[read_index++];
            ++code;
        }
    }
    output[code_index] = code;
    output[write_index++] = 0;
    return write_index;
}

static size_t make_raw_frame(uint8_t version,
                             uint8_t type,
                             uint8_t declared_payload_len,
                             const uint8_t *payload,
                             size_t actual_payload_len,
                             uint8_t *output,
                             size_t output_capacity)
{
    uint8_t decoded[SERIAL_FRAME_MAX_DECODED_SIZE];
    decoded[0] = version;
    decoded[1] = type;
    decoded[2] = declared_payload_len;
    memcpy(&decoded[3], payload, actual_payload_len);
    const uint16_t crc = serial_crc16_ccitt_false(decoded, 3 + actual_payload_len);
    decoded[3 + actual_payload_len] = (uint8_t)crc;
    decoded[4 + actual_payload_len] = (uint8_t)(crc >> 8);
    return test_cobs_encode(decoded,
                            5 + actual_payload_len,
                            output,
                            output_capacity);
}

TEST_CASE("serial CRC and input framing match golden bytes", "[input_transports]")
{
    static const uint8_t check[] = "123456789";
    TEST_ASSERT_EQUAL_HEX16(0x29b1,
                            serial_crc16_ccitt_false(check, sizeof(check) - 1));

    uint8_t encoded[SERIAL_FRAME_MAX_ENCODED_SIZE];
    const size_t encoded_len = serial_frame_encode(SERIAL_FRAME_TYPE_INPUT,
                                                    k_input_payload,
                                                    sizeof(k_input_payload),
                                                    encoded,
                                                    sizeof(encoded));
    TEST_ASSERT_EQUAL_UINT32(sizeof(k_input_frame), encoded_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(k_input_frame, encoded, sizeof(k_input_frame));
}

TEST_CASE("serial status framing is exact", "[input_transports]")
{
    static const uint8_t status[20] = {0x01};
    static const uint8_t expected[27] = {
        0x05, 0x01, 0x81, 0x14, 0x01, 0x01, 0x01, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x01, 0x03, 0xd5, 0xa8, 0x00,
    };
    uint8_t encoded[SERIAL_FRAME_MAX_ENCODED_SIZE];

    const size_t encoded_len = serial_frame_encode(SERIAL_FRAME_TYPE_STATUS,
                                                    status,
                                                    sizeof(status),
                                                    encoded,
                                                    sizeof(encoded));
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), encoded_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, encoded, sizeof(expected));
}

TEST_CASE("serial parser accepts every fragmentation boundary", "[input_transports]")
{
    for (size_t split = 0; split <= sizeof(k_input_frame); ++split) {
        serial_parser_t parser;
        callback_capture_t capture = {0};
        serial_parser_init(&parser, capture_frame, &capture);

        serial_parser_feed(&parser, k_input_frame, split);
        if (split < sizeof(k_input_frame)) {
            TEST_ASSERT_EQUAL_UINT32(0, capture.calls);
        }
        serial_parser_feed(&parser,
                           &k_input_frame[split],
                           sizeof(k_input_frame) - split);

        TEST_ASSERT_EQUAL_UINT32(1, capture.calls);
        TEST_ASSERT_EQUAL_HEX8(SERIAL_FRAME_TYPE_INPUT, capture.types[0]);
        TEST_ASSERT_EQUAL_UINT32(sizeof(k_input_payload), capture.lengths[0]);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(k_input_payload,
                                      capture.payloads[0],
                                      sizeof(k_input_payload));
    }
}

TEST_CASE("serial parser emits coalesced frames independently", "[input_transports]")
{
    uint8_t stream[sizeof(k_input_frame) * 2];
    memcpy(stream, k_input_frame, sizeof(k_input_frame));
    memcpy(&stream[sizeof(k_input_frame)], k_input_frame, sizeof(k_input_frame));

    serial_parser_t parser;
    callback_capture_t capture = {0};
    serial_parser_init(&parser, capture_frame, &capture);
    serial_parser_feed(&parser, stream, sizeof(stream));

    TEST_ASSERT_EQUAL_UINT32(2, capture.calls);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(k_input_payload,
                                  capture.payloads[0],
                                  sizeof(k_input_payload));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(k_input_payload,
                                  capture.payloads[1],
                                  sizeof(k_input_payload));
}

TEST_CASE("serial parser resynchronizes after noise and overlong data", "[input_transports]")
{
    static const uint8_t boot_noise[] = "rst:0x1 (POWERON_RESET)";
    uint8_t overlong[SERIAL_FRAME_MAX_ENCODED_SIZE + 8];
    memset(overlong, 0x55, sizeof(overlong));

    serial_parser_t parser;
    callback_capture_t capture = {0};
    serial_parser_init(&parser, capture_frame, &capture);
    serial_parser_feed(&parser, boot_noise, sizeof(boot_noise) - 1);
    const uint8_t delimiter = 0;
    serial_parser_feed(&parser, &delimiter, 1);
    serial_parser_feed(&parser, overlong, sizeof(overlong));
    serial_parser_feed(&parser, &delimiter, 1);
    serial_parser_feed(&parser, k_input_frame, sizeof(k_input_frame));

    TEST_ASSERT_EQUAL_UINT32(1, capture.calls);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(k_input_payload,
                                  capture.payloads[0],
                                  sizeof(k_input_payload));
}

TEST_CASE("serial parser rejects wrong header version type and length", "[input_transports]")
{
    uint8_t malformed[SERIAL_FRAME_MAX_ENCODED_SIZE];
    serial_parser_t parser;
    callback_capture_t capture = {0};
    serial_parser_init(&parser, capture_frame, &capture);

    size_t len = make_raw_frame(2,
                                SERIAL_FRAME_TYPE_INPUT,
                                sizeof(k_input_payload),
                                k_input_payload,
                                sizeof(k_input_payload),
                                malformed,
                                sizeof(malformed));
    serial_parser_feed(&parser, malformed, len);

    len = make_raw_frame(SERIAL_FRAME_VERSION,
                         0x55,
                         sizeof(k_input_payload),
                         k_input_payload,
                         sizeof(k_input_payload),
                         malformed,
                         sizeof(malformed));
    serial_parser_feed(&parser, malformed, len);

    len = make_raw_frame(SERIAL_FRAME_VERSION,
                         SERIAL_FRAME_TYPE_INPUT,
                         sizeof(k_input_payload) - 1,
                         k_input_payload,
                         sizeof(k_input_payload) - 1,
                         malformed,
                         sizeof(malformed));
    serial_parser_feed(&parser, malformed, len);

    len = make_raw_frame(SERIAL_FRAME_VERSION,
                         SERIAL_FRAME_TYPE_INPUT,
                         sizeof(k_input_payload),
                         k_input_payload,
                         sizeof(k_input_payload) - 1,
                         malformed,
                         sizeof(malformed));
    serial_parser_feed(&parser, malformed, len);

    TEST_ASSERT_EQUAL_UINT32(0, capture.calls);
}

TEST_CASE("serial parser rejects bad CRC and accepts the following frame", "[input_transports]")
{
    uint8_t stream[sizeof(k_input_frame) * 2];
    memcpy(stream, k_input_frame, sizeof(k_input_frame));
    stream[10] ^= 0x01;
    memcpy(&stream[sizeof(k_input_frame)], k_input_frame, sizeof(k_input_frame));

    serial_parser_t parser;
    callback_capture_t capture = {0};
    serial_parser_init(&parser, capture_frame, &capture);
    serial_parser_feed(&parser, stream, sizeof(stream));

    TEST_ASSERT_EQUAL_UINT32(1, capture.calls);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(k_input_payload,
                                  capture.payloads[0],
                                  sizeof(k_input_payload));
}
