#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "switch2_protocol.h"
#include "unity.h"

static const uint8_t s_a1[16] = {
    0x35,0x03,0xe9,0x29,0x82,0x87,0x71,0x24,
    0xbe,0xa8,0x0c,0x66,0x46,0x15,0x83,0x4b,
};
static const uint8_t s_ltk[16] = {
    0x69,0xf5,0x07,0x50,0xae,0x58,0x74,0xc5,
    0x04,0x83,0x6f,0x43,0x82,0x0f,0xdc,0x5b,
};
static const uint8_t s_a2[16] = {
    0x6f,0xc6,0xdf,0x8a,0xd8,0xfe,0xdf,0x15,
    0xbb,0x8c,0x15,0xe9,0x1f,0x32,0x05,0x44,
};
static const uint8_t s_b2[16] = {
    0x13,0x4c,0x97,0xf5,0x11,0xb9,0xb6,0xdd,
    0x4d,0x86,0xfd,0x40,0xf5,0x36,0xe9,0xed,
};

TEST_CASE("Switch2 derives documented A1 xor B1 LTK", "[switch2_pairing]")
{
    uint8_t ltk[16];
    switch2_protocol_derive_ltk(s_a1, ltk);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_ltk, ltk, sizeof(ltk));
}

TEST_CASE("Switch2 AES challenge uses documented reversed byte order", "[switch2_pairing]")
{
    uint8_t b2[16];
    TEST_ASSERT_TRUE(switch2_protocol_pairing_challenge(s_ltk, s_a2, b2));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_b2, b2, sizeof(b2));
}

TEST_CASE("Switch2 paired record round trips exact identity and key", "[switch2_pairing]")
{
    switch2_pairing_record_t record = {
        .controller_address = {0x78,0x81,0x8c,0x98,0xe2,0x55},
        .console_address = {0x81,0xeb,0x3a,0xeb,0xf1,0x48},
        .paired = true,
    };
    memcpy(record.ltk, s_ltk, sizeof(record.ltk));
    uint8_t bytes[SWITCH2_PAIRING_RECORD_SIZE];
    switch2_protocol_pairing_record_encode(&record, bytes);
    static const uint8_t magic[4] = {'S','2','P','G'};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(magic, bytes, sizeof(magic));
    TEST_ASSERT_EQUAL_UINT8(1, bytes[4]);
    TEST_ASSERT_EQUAL_UINT8(1, bytes[11]);

    switch2_pairing_record_t decoded;
    TEST_ASSERT_TRUE(switch2_protocol_pairing_record_decode(bytes, sizeof(bytes), &decoded));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(record.controller_address, decoded.controller_address, 6);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(record.console_address, decoded.console_address, 6);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(record.ltk, decoded.ltk, 16);
    TEST_ASSERT_TRUE(decoded.paired);
}

TEST_CASE("Switch2 unpaired record preserves identity and zeroes pairing fields", "[switch2_pairing]")
{
    switch2_pairing_record_t record = {
        .controller_address = {0x78,0x81,0x8c,1,2,3},
        .console_address = {1,2,3,4,5,6},
        .ltk = {1},
        .paired = false,
    };
    uint8_t bytes[SWITCH2_PAIRING_RECORD_SIZE];
    switch2_protocol_pairing_record_encode(&record, bytes);
    TEST_ASSERT_EACH_EQUAL_HEX8(0, &bytes[12], 22);
    switch2_pairing_record_t decoded;
    TEST_ASSERT_TRUE(switch2_protocol_pairing_record_decode(bytes, sizeof(bytes), &decoded));
    TEST_ASSERT_FALSE(decoded.paired);
}

TEST_CASE("Switch2 record rejects partial, version, CRC, OUI and zero paired fields", "[switch2_pairing]")
{
    switch2_pairing_record_t record = {
        .controller_address = {0x78,0x81,0x8c,1,2,3},
        .console_address = {1,2,3,4,5,6},
        .ltk = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16},
        .paired = true,
    };
    uint8_t bytes[SWITCH2_PAIRING_RECORD_SIZE];
    switch2_protocol_pairing_record_encode(&record, bytes);
    switch2_pairing_record_t decoded;
    for (size_t len = 0; len < sizeof(bytes); ++len) {
        TEST_ASSERT_FALSE(switch2_protocol_pairing_record_decode(bytes, len, &decoded));
    }

    uint8_t changed[SWITCH2_PAIRING_RECORD_SIZE];
    memcpy(changed, bytes, sizeof(changed));
    changed[4] = 2;
    TEST_ASSERT_FALSE(switch2_protocol_pairing_record_decode(changed, sizeof(changed), &decoded));
    memcpy(changed, bytes, sizeof(changed));
    changed[20] ^= 1;
    TEST_ASSERT_FALSE(switch2_protocol_pairing_record_decode(changed, sizeof(changed), &decoded));
    memcpy(changed, bytes, sizeof(changed));
    changed[5] = 0;
    TEST_ASSERT_FALSE(switch2_protocol_pairing_record_decode(changed, sizeof(changed), &decoded));
    memcpy(changed, bytes, sizeof(changed));
    memset(&changed[12], 0, 6);
    TEST_ASSERT_FALSE(switch2_protocol_pairing_record_decode(changed, sizeof(changed), &decoded));
}
