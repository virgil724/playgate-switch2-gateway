#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "switch2_protocol.h"
#include "unity.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static const uint8_t s_controller[6] = {0x78,0x81,0x8c,0x98,0xe2,0x55};

static size_t make_request(switch2_command_endpoint_t endpoint,
                           uint8_t command, uint8_t subcommand,
                           const uint8_t *data, size_t data_len,
                           uint8_t out[SWITCH2_COMMAND_RESPONSE_MAX_SIZE])
{
    const size_t prefix = endpoint == SWITCH2_COMMAND_COMBINED
                              ? SWITCH2_COMMAND_VIBRATION_PREFIX_SIZE : 0;
    memset(out, 0, SWITCH2_COMMAND_RESPONSE_MAX_SIZE);
    out[prefix + 0] = command;
    out[prefix + 1] = 0x91;
    out[prefix + 2] = 0x01;
    out[prefix + 3] = subcommand;
    out[prefix + 5] = (uint8_t)data_len;
    if (data_len != 0) {
        memcpy(&out[prefix + 8], data, data_len);
    }
    return prefix + 8 + data_len;
}

static switch2_command_result_t process(switch2_protocol_t *protocol,
                                        switch2_command_endpoint_t endpoint,
                                        const uint8_t *request, size_t request_len,
                                        uint8_t *response, size_t *response_len,
                                        switch2_command_effects_t *effects)
{
    return switch2_protocol_process_command(protocol, 7, endpoint,
                                             request, request_len,
                                             response,
                                             SWITCH2_COMMAND_RESPONSE_MAX_SIZE,
                                             response_len, effects);
}

static void init_protocol(switch2_protocol_t *protocol)
{
    switch2_protocol_init(protocol, s_controller);
    switch2_protocol_begin_connection(protocol, 7);
}

static void assert_response(const uint8_t *response, size_t response_len,
                            switch2_command_endpoint_t endpoint,
                            uint8_t command, uint8_t subcommand,
                            const uint8_t *payload, size_t payload_len)
{
    const size_t prefix = endpoint == SWITCH2_COMMAND_COMBINED ? 14 : 0;
    TEST_ASSERT_EQUAL_UINT32(prefix + 8 + payload_len, response_len);
    if (prefix != 0) {
        TEST_ASSERT_EACH_EQUAL_HEX8(0, response, prefix);
    }
    const uint8_t expected_header[8] = {
        command,0x01,0x01,subcommand,0x10,0x78,0x00,0x00,
    };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_header, &response[prefix], 8);
    if (payload_len != 0) {
        TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, &response[prefix + 8], payload_len);
    }
}

TEST_CASE("Switch2 basic command fixtures have exact response headers and payloads", "[switch2_commands]")
{
    static const uint8_t feature_request[4] = {0x2f,0,0,0};
    const struct {
        uint8_t command;
        uint8_t subcommand;
        const uint8_t *data;
        size_t data_len;
        const uint8_t *response;
        size_t response_len;
    } cases[] = {
        {0x01,0x0c,NULL,0,(const uint8_t[]){0x61,0x12,0x50,0x10},4},
        {0x03,0x08,NULL,0,NULL,0},
        {0x07,0x01,NULL,0,(const uint8_t[]){0x00},1},
        {0x0c,0x01,feature_request,4,
         (const uint8_t[]){0,0,0,0,0x07,0x07,0x01,0,0,0x03,0,0},12},
        {0x0c,0x02,feature_request,4,(const uint8_t[]){0,0,0,0},4},
        {0x0c,0x03,feature_request,4,(const uint8_t[]){0,0,0,0},4},
        {0x0c,0x04,feature_request,4,(const uint8_t[]){0,0,0,0},4},
        {0x0c,0x05,feature_request,4,(const uint8_t[]){0,0,0,0},4},
        {0x10,0x01,NULL,0,
         (const uint8_t[]){0x02,0x01,0x04,0x02,0x0c,0,0,0,0,0x02,0x03,0},12},
        {0x11,0x01,NULL,0,(const uint8_t[]){0x01,0,0,0},4},
        {0x11,0x03,NULL,0,
         (const uint8_t[]){0x01,0x20,0x03,0,0,0x0a,0xe8,0x1c,
                           0x3b,0x79,0x7d,0x8b,0x3a,0x0a,0xe8,0x9c,
                           0x42,0x58,0xa0,0x0b,0x42,0x0a,0xe8,0x9c,
                           0x41,0x58,0xa0,0x0b,0x41},29},
        {0x16,0x01,NULL,0,(const uint8_t[24]){0},24},
        {0x18,0x01,NULL,0,(const uint8_t[]){0,0,0x40,0xf0,0,0,0x60,0},8},
        {0x09,0x01,NULL,0,NULL,0},
        {0x0a,0x01,NULL,0,NULL,0},
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); ++i) {
        switch2_protocol_t protocol;
        init_protocol(&protocol);
        uint8_t request[SWITCH2_COMMAND_RESPONSE_MAX_SIZE];
        uint8_t response[SWITCH2_COMMAND_RESPONSE_MAX_SIZE];
        size_t response_len;
        switch2_command_effects_t effects;
        const size_t request_len = make_request(SWITCH2_COMMAND_BASIC,
            cases[i].command, cases[i].subcommand, cases[i].data,
            cases[i].data_len, request);
        TEST_ASSERT_EQUAL_INT(SWITCH2_COMMAND_OK,
            process(&protocol, SWITCH2_COMMAND_BASIC, request, request_len,
                    response, &response_len, &effects));
        assert_response(response, response_len, SWITCH2_COMMAND_BASIC,
                        cases[i].command, cases[i].subcommand,
                        cases[i].response, cases[i].response_len);
    }
}

TEST_CASE("Switch2 direct pairing and clear commands expose explicit effects", "[switch2_commands]")
{
    static const uint8_t direct[22] = {
        0x81,0xeb,0x3a,0xeb,0xf1,0x48,
        1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
    };
    switch2_protocol_t protocol;
    init_protocol(&protocol);
    uint8_t request[128], response[128];
    size_t response_len;
    switch2_command_effects_t effects;
    size_t request_len = make_request(SWITCH2_COMMAND_BASIC,0x03,0x07,
                                      direct,sizeof(direct),request);
    TEST_ASSERT_EQUAL_INT(SWITCH2_COMMAND_OK,
        process(&protocol,SWITCH2_COMMAND_BASIC,request,request_len,
                response,&response_len,&effects));
    TEST_ASSERT_TRUE(effects.direct_pairing_received);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(direct,effects.console_address,6);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&direct[6],effects.ltk,16);

    request_len = make_request(SWITCH2_COMMAND_BASIC,0x03,0x08,NULL,0,request);
    TEST_ASSERT_EQUAL_INT(SWITCH2_COMMAND_OK,
        process(&protocol,SWITCH2_COMMAND_BASIC,request,request_len,
                response,&response_len,&effects));
    TEST_ASSERT_TRUE(effects.clear_pairing);
}

TEST_CASE("Switch2 memory read fixture is exact and unmapped bytes are 0xff", "[switch2_commands]")
{
    const uint8_t data[8] = {0x14,0x7e,0,0,0x38,0x30,0x01,0};
    const uint8_t expected[28] = {
        0x14,0,0,0,0x38,0x30,0x01,0,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0x3b,0xe0,0xd3,0x41,0xc6,0x60,0x6a,0xbc,
        0x4d,0xd7,0xa2,0xbb,
    };
    switch2_protocol_t protocol;
    init_protocol(&protocol);
    uint8_t request[128], response[128];
    size_t response_len;
    switch2_command_effects_t effects;
    const size_t request_len = make_request(SWITCH2_COMMAND_BASIC,0x02,0x04,
                                             data,sizeof(data),request);
    TEST_ASSERT_EQUAL_INT(SWITCH2_COMMAND_OK,
        process(&protocol,SWITCH2_COMMAND_BASIC,request,request_len,
                response,&response_len,&effects));
    assert_response(response,response_len,SWITCH2_COMMAND_BASIC,0x02,0x04,
                    expected,sizeof(expected));
}

TEST_CASE("Switch2 pairing fixtures enforce address key challenge finalise order", "[switch2_commands]")
{
    static const uint8_t addresses[14] = {
        0x00,0x02, 0x81,0xeb,0x3a,0xeb,0xf1,0x48,
        0x80,0xeb,0x3a,0xeb,0xf1,0x48,
    };
    static const uint8_t exchange_key[17] = {
        0x00,0x35,0x03,0xe9,0x29,0x82,0x87,0x71,0x24,
        0xbe,0xa8,0x0c,0x66,0x46,0x15,0x83,0x4b,
    };
    static const uint8_t challenge[17] = {
        0x00,0x6f,0xc6,0xdf,0x8a,0xd8,0xfe,0xdf,0x15,
        0xbb,0x8c,0x15,0xe9,0x1f,0x32,0x05,0x44,
    };
    static const uint8_t ltk[16] = {
        0x69,0xf5,0x07,0x50,0xae,0x58,0x74,0xc5,
        0x04,0x83,0x6f,0x43,0x82,0x0f,0xdc,0x5b,
    };
    static const uint8_t b1_response[17] = {
        0x01,0x5c,0xf6,0xee,0x79,0x2c,0xdf,0x05,0xe1,
        0xba,0x2b,0x63,0x25,0xc4,0x1a,0x5f,0x10,
    };
    static const uint8_t b2_response[17] = {
        0x01,0x13,0x4c,0x97,0xf5,0x11,0xb9,0xb6,0xdd,
        0x4d,0x86,0xfd,0x40,0xf5,0x36,0xe9,0xed,
    };
    switch2_protocol_t protocol;
    init_protocol(&protocol);
    uint8_t request[128], response[128];
    size_t response_len;
    switch2_command_effects_t effects;

    size_t request_len = make_request(SWITCH2_COMMAND_BASIC,0x15,0x01,
                                      addresses,sizeof(addresses),request);
    TEST_ASSERT_EQUAL_INT(SWITCH2_COMMAND_OK,
        process(&protocol,SWITCH2_COMMAND_BASIC,request,request_len,response,&response_len,&effects));
    assert_response(response,response_len,SWITCH2_COMMAND_BASIC,0x15,0x01,
                    (const uint8_t[]){0x01,0x04,0x01,0x55,0xe2,0x98,0x8c,0x81,0x78},9);

    request_len = make_request(SWITCH2_COMMAND_BASIC,0x15,0x04,
                               exchange_key,sizeof(exchange_key),request);
    TEST_ASSERT_EQUAL_INT(SWITCH2_COMMAND_OK,
        process(&protocol,SWITCH2_COMMAND_BASIC,request,request_len,response,&response_len,&effects));
    assert_response(response,response_len,SWITCH2_COMMAND_BASIC,0x15,0x04,b1_response,sizeof(b1_response));

    request_len = make_request(SWITCH2_COMMAND_BASIC,0x15,0x02,
                               challenge,sizeof(challenge),request);
    TEST_ASSERT_EQUAL_INT(SWITCH2_COMMAND_OK,
        process(&protocol,SWITCH2_COMMAND_BASIC,request,request_len,response,&response_len,&effects));
    assert_response(response,response_len,SWITCH2_COMMAND_BASIC,0x15,0x02,b2_response,sizeof(b2_response));

    request_len = make_request(SWITCH2_COMMAND_BASIC,0x15,0x03,
                               (const uint8_t[]){0},1,request);
    TEST_ASSERT_EQUAL_INT(SWITCH2_COMMAND_OK,
        process(&protocol,SWITCH2_COMMAND_BASIC,request,request_len,response,&response_len,&effects));
    TEST_ASSERT_TRUE(effects.pairing_finalized);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&addresses[2],effects.console_address,6);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ltk,effects.ltk,16);
    assert_response(response,response_len,SWITCH2_COMMAND_BASIC,0x15,0x03,
                    (const uint8_t[]){1},1);
}

TEST_CASE("Switch2 combined endpoint validates prefix and adds exact response prefix", "[switch2_commands]")
{
    switch2_protocol_t protocol;
    init_protocol(&protocol);
    uint8_t request[128], response[128];
    size_t response_len;
    switch2_command_effects_t effects;
    const size_t request_len = make_request(SWITCH2_COMMAND_COMBINED,0x10,0x01,NULL,0,request);
    TEST_ASSERT_EQUAL_INT(SWITCH2_COMMAND_OK,
        process(&protocol,SWITCH2_COMMAND_COMBINED,request,request_len,response,&response_len,&effects));
    assert_response(response,response_len,SWITCH2_COMMAND_COMBINED,0x10,0x01,
        (const uint8_t[]){0x02,0x01,0x04,0x02,0x0c,0,0,0,0,0x02,0x03,0},12);
    request[0] = 1;
    TEST_ASSERT_EQUAL_INT(SWITCH2_COMMAND_MALFORMED,
        process(&protocol,SWITCH2_COMMAND_COMBINED,request,request_len,response,&response_len,&effects));
}

TEST_CASE("Switch2 rejects header mismatch, truncation, oversize and address overflow", "[switch2_commands]")
{
    switch2_protocol_t protocol;
    uint8_t request[128], response[128];
    size_t response_len;
    switch2_command_effects_t effects;
    init_protocol(&protocol);
    size_t request_len = make_request(SWITCH2_COMMAND_BASIC,0x10,0x01,NULL,0,request);
    for (size_t len = 0; len < 8; ++len) {
        TEST_ASSERT_EQUAL_INT(SWITCH2_COMMAND_MALFORMED,
            process(&protocol,SWITCH2_COMMAND_BASIC,request,len,response,&response_len,&effects));
    }
    request[5] = 1;
    TEST_ASSERT_EQUAL_INT(SWITCH2_COMMAND_MALFORMED,
        process(&protocol,SWITCH2_COMMAND_BASIC,request,request_len,response,&response_len,&effects));
    request[5] = 0; request[1] = 0x01;
    TEST_ASSERT_EQUAL_INT(SWITCH2_COMMAND_MALFORMED,
        process(&protocol,SWITCH2_COMMAND_BASIC,request,request_len,response,&response_len,&effects));
    request[1] = 0x91; request[2] = 0;
    TEST_ASSERT_EQUAL_INT(SWITCH2_COMMAND_MALFORMED,
        process(&protocol,SWITCH2_COMMAND_BASIC,request,request_len,response,&response_len,&effects));

    const uint8_t memory_oversize[8] = {0x50,0x7e,0,0,0,0,0,0};
    request_len = make_request(SWITCH2_COMMAND_BASIC,0x02,0x04,memory_oversize,8,request);
    TEST_ASSERT_EQUAL_INT(SWITCH2_COMMAND_MALFORMED,
        process(&protocol,SWITCH2_COMMAND_BASIC,request,request_len,response,&response_len,&effects));
    const uint8_t memory_overflow[8] = {2,0x7e,0,0,0xff,0xff,0xff,0xff};
    request_len = make_request(SWITCH2_COMMAND_BASIC,0x02,0x04,memory_overflow,8,request);
    TEST_ASSERT_EQUAL_INT(SWITCH2_COMMAND_MALFORMED,
        process(&protocol,SWITCH2_COMMAND_BASIC,request,request_len,response,&response_len,&effects));
}

TEST_CASE("Switch2 rejects every truncated pairing data offset and wrong state", "[switch2_commands]")
{
    uint8_t full[17] = {0};
    for (size_t i = 1; i < sizeof(full); ++i) {
        full[i] = (uint8_t)i;
    }
    uint8_t request[128], response[128];
    size_t response_len;
    switch2_command_effects_t effects;
    for (size_t data_len = 0; data_len < sizeof(full); ++data_len) {
        switch2_protocol_t protocol;
        init_protocol(&protocol);
        const size_t request_len = make_request(SWITCH2_COMMAND_BASIC,0x15,0x04,
                                                 full,data_len,request);
        TEST_ASSERT_EQUAL_INT(SWITCH2_COMMAND_MALFORMED,
            process(&protocol,SWITCH2_COMMAND_BASIC,request,request_len,response,&response_len,&effects));
    }

    switch2_protocol_t protocol;
    init_protocol(&protocol);
    size_t request_len = make_request(SWITCH2_COMMAND_BASIC,0x15,0x04,full,sizeof(full),request);
    TEST_ASSERT_EQUAL_INT(SWITCH2_COMMAND_STATE_REJECTED,
        process(&protocol,SWITCH2_COMMAND_BASIC,request,request_len,response,&response_len,&effects));
    request_len = make_request(SWITCH2_COMMAND_BASIC,0x15,0x02,full,sizeof(full),request);
    TEST_ASSERT_EQUAL_INT(SWITCH2_COMMAND_STATE_REJECTED,
        process(&protocol,SWITCH2_COMMAND_BASIC,request,request_len,response,&response_len,&effects));
    request_len = make_request(SWITCH2_COMMAND_BASIC,0x15,0x03,(const uint8_t[]){0},1,request);
    TEST_ASSERT_EQUAL_INT(SWITCH2_COMMAND_STATE_REJECTED,
        process(&protocol,SWITCH2_COMMAND_BASIC,request,request_len,response,&response_len,&effects));

    request_len = make_request(SWITCH2_COMMAND_BASIC,0xee,0xaa,NULL,0,request);
    TEST_ASSERT_EQUAL_INT(SWITCH2_COMMAND_OK,
        process(&protocol,SWITCH2_COMMAND_BASIC,request,request_len,response,&response_len,&effects));
    assert_response(response,response_len,SWITCH2_COMMAND_BASIC,0xee,0xaa,NULL,0);
}

TEST_CASE("Switch2 confirms 0x09 and 0x0a payloads with an empty body", "[switch2_commands]")
{
    /* Captured on hardware: after encryption the console writes 45 bytes to
     * handle 0x0016, i.e. a 33-byte vibration prefix, the 8-byte header
     * 0a 91 01 02 00 04 00 00 and a 4-byte payload. Upstream registers no
     * handler for 0x09/0x0a and confirms from its default branch with an
     * empty body, so a non-zero payload length is legitimate, not an error. */
    static const uint8_t header[8] = {0x0a,0x91,0x01,0x02,0x00,0x04,0x00,0x00};
    static const uint8_t payload[4] = {0,0,0,0};

    for (uint8_t command = 0x09; command <= 0x0a; ++command) {
        switch2_protocol_t protocol;
        init_protocol(&protocol);
        uint8_t request[SWITCH2_COMMAND_RESPONSE_MAX_SIZE];
        uint8_t response[SWITCH2_COMMAND_RESPONSE_MAX_SIZE];
        size_t response_len;
        switch2_command_effects_t effects;
        memset(request, 0, sizeof(request));
        memcpy(&request[SWITCH2_COMMAND_VIBRATION_PREFIX_SIZE], header, sizeof(header));
        request[SWITCH2_COMMAND_VIBRATION_PREFIX_SIZE] = command;
        memcpy(&request[SWITCH2_COMMAND_VIBRATION_PREFIX_SIZE + 8], payload, sizeof(payload));
        const size_t request_len =
            SWITCH2_COMMAND_VIBRATION_PREFIX_SIZE + sizeof(header) + sizeof(payload);
        TEST_ASSERT_EQUAL_INT(SWITCH2_COMMAND_OK,
            process(&protocol,SWITCH2_COMMAND_COMBINED,request,request_len,
                    response,&response_len,&effects));
        assert_response(response,response_len,SWITCH2_COMMAND_COMBINED,
                        command,0x02,NULL,0);
    }
}
