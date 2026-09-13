#include <stdint.h>
#include <string.h>

#include "switch2_protocol.h"
#include "unity.h"

static const uint8_t s_standard[SWITCH2_MANUFACTURER_DATA_SIZE] = {
    0x53,0x05,0x01,0x00,0x03,0x7e,0x05,0x69,0x20,
    0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};

TEST_CASE("Switch2 standard advertisement is the exact Nintendo fixture", "[switch2_advertising]")
{
    uint8_t out[SWITCH2_MANUFACTURER_DATA_SIZE];
    memset(out, 0xa5, sizeof(out));
    switch2_protocol_encode_manufacturer_data(SWITCH2_ADV_STANDARD,
                                               (const uint8_t[]){1,2,3,4,5,6}, out);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_standard, out, sizeof(out));
    TEST_ASSERT_EQUAL_HEX16(0x0553, (uint16_t)out[0] | ((uint16_t)out[1] << 8));
    TEST_ASSERT_EQUAL_HEX16(0x057e, (uint16_t)out[5] | ((uint16_t)out[6] << 8));
    TEST_ASSERT_EQUAL_HEX16(0x2069, (uint16_t)out[7] | ((uint16_t)out[8] << 8));
}

TEST_CASE("Switch2 reconnect advertisement carries the console address", "[switch2_advertising]")
{
    static const uint8_t address[6] = {0x5f,0x11,0x85,0xeb,0xf1,0x48};
    uint8_t expected[SWITCH2_MANUFACTURER_DATA_SIZE];
    memcpy(expected, s_standard, sizeof(expected));
    memcpy(&expected[12], address, sizeof(address));
    uint8_t out[SWITCH2_MANUFACTURER_DATA_SIZE];
    switch2_protocol_encode_manufacturer_data(SWITCH2_ADV_RECONNECT, address, out);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, sizeof(out));
}

TEST_CASE("Switch2 wake advertisement has exact 0x81 opcode and address", "[switch2_advertising]")
{
    static const uint8_t address[6] = {0x5f,0x11,0x85,0xeb,0xf1,0x48};
    uint8_t expected[SWITCH2_MANUFACTURER_DATA_SIZE];
    memcpy(expected, s_standard, sizeof(expected));
    expected[11] = 0x81;
    memcpy(&expected[12], address, sizeof(address));
    uint8_t out[SWITCH2_MANUFACTURER_DATA_SIZE];
    switch2_protocol_encode_manufacturer_data(SWITCH2_ADV_WAKE, address, out);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, sizeof(out));
    TEST_ASSERT_EQUAL_HEX8(0x0f, out[18]);
}
