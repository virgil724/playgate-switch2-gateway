#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "switch2_protocol.h"
#include "unity.h"

static uint16_t unpack_x(const uint8_t stick[3])
{
    return (uint16_t)stick[0] | ((uint16_t)(stick[1] & 0x0f) << 8);
}

static uint16_t unpack_y(const uint8_t stick[3])
{
    return (uint16_t)(stick[1] >> 4) | ((uint16_t)stick[2] << 4);
}

TEST_CASE("Switch2 neutral input report is an exact 63-byte fixture", "[switch2_protocol]")
{
    static const uint8_t expected[SWITCH2_INPUT_REPORT_SIZE] = {
        0x7f,0x20,0x00,0x00,0x00,0x00,0x08,0x80,
        0x00,0x08,0x80,0x30,0x00,0x00,0x28,
    };
    controller_state_t state = {0};
    uint8_t report[SWITCH2_INPUT_REPORT_SIZE];
    memset(report, 0xa5, sizeof(report));
    switch2_protocol_encode_input(0x7f, 0, &state, report);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, report, sizeof(expected));
}

TEST_CASE("Switch2 maps every PlayGate button to the exact Pro2 bit", "[switch2_protocol]")
{
    static const struct {
        uint32_t input;
        uint8_t byte;
        uint8_t mask;
    } cases[] = {
        {PLAYGATE_BUTTON_B,2,0x01},{PLAYGATE_BUTTON_A,2,0x02},
        {PLAYGATE_BUTTON_Y,2,0x04},{PLAYGATE_BUTTON_X,2,0x08},
        {PLAYGATE_BUTTON_R,2,0x10},{PLAYGATE_BUTTON_ZR,2,0x20},
        {PLAYGATE_BUTTON_PLUS,2,0x40},{PLAYGATE_BUTTON_R_STICK,2,0x80},
        {PLAYGATE_BUTTON_DPAD_DOWN,3,0x01},{PLAYGATE_BUTTON_DPAD_RIGHT,3,0x02},
        {PLAYGATE_BUTTON_DPAD_LEFT,3,0x04},{PLAYGATE_BUTTON_DPAD_UP,3,0x08},
        {PLAYGATE_BUTTON_L,3,0x10},{PLAYGATE_BUTTON_ZL,3,0x20},
        {PLAYGATE_BUTTON_MINUS,3,0x40},{PLAYGATE_BUTTON_L_STICK,3,0x80},
        {PLAYGATE_BUTTON_HOME,4,0x01},{PLAYGATE_BUTTON_CAPTURE,4,0x02},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        controller_state_t state = {.buttons = cases[i].input};
        uint8_t report[SWITCH2_INPUT_REPORT_SIZE];
        switch2_protocol_encode_input(0, 0, &state, report);
        TEST_ASSERT_EQUAL_HEX8(cases[i].mask, report[cases[i].byte]);
        TEST_ASSERT_EQUAL_HEX8(0, report[2] & (cases[i].byte == 2 ? (uint8_t)~cases[i].mask : 0xff));
        TEST_ASSERT_EQUAL_HEX8(0, report[3] & (cases[i].byte == 3 ? (uint8_t)~cases[i].mask : 0xff));
        TEST_ASSERT_EQUAL_HEX8(0, report[4] & (cases[i].byte == 4 ? (uint8_t)~cases[i].mask : 0xff));
    }
}

TEST_CASE("Switch2 axes use asymmetric halves and invert Y only at encode", "[switch2_protocol]")
{
    controller_state_t state = {
        .left_x = PLAYGATE_AXIS_MIN, .left_y = PLAYGATE_AXIS_MIN,
        .right_x = PLAYGATE_AXIS_MAX, .right_y = PLAYGATE_AXIS_MAX,
    };
    uint8_t report[SWITCH2_INPUT_REPORT_SIZE];
    switch2_protocol_encode_input(0, 0, &state, report);
    TEST_ASSERT_EQUAL_UINT16(0, unpack_x(&report[5]));
    TEST_ASSERT_EQUAL_UINT16(4095, unpack_y(&report[5]));
    TEST_ASSERT_EQUAL_UINT16(4095, unpack_x(&report[8]));
    TEST_ASSERT_EQUAL_UINT16(0, unpack_y(&report[8]));

    memset(&state, 0, sizeof(state));
    switch2_protocol_encode_input(0, 0, &state, report);
    TEST_ASSERT_EQUAL_UINT16(2048, unpack_x(&report[5]));
    TEST_ASSERT_EQUAL_UINT16(2048, unpack_y(&report[5]));
    TEST_ASSERT_EQUAL_UINT16(2048, unpack_x(&report[8]));
    TEST_ASSERT_EQUAL_UINT16(2048, unpack_y(&report[8]));
}

TEST_CASE("Switch2 report counter wraps and feature 5 selects 0x38", "[switch2_protocol]")
{
    controller_state_t state = {0};
    uint8_t report[SWITCH2_INPUT_REPORT_SIZE];
    switch2_protocol_encode_input(UINT8_MAX, 0x20, &state, report);
    TEST_ASSERT_EQUAL_HEX8(0xff, report[0]);
    TEST_ASSERT_EQUAL_HEX8(0x38, report[0x0b]);
    switch2_protocol_encode_input((uint8_t)(UINT8_MAX + 1u), 0, &state, report);
    TEST_ASSERT_EQUAL_HEX8(0x00, report[0]);
    TEST_ASSERT_EQUAL_HEX8(0x30, report[0x0b]);
}

TEST_CASE("Switch2 report overwrites every prefilled byte deterministically", "[switch2_protocol]")
{
    controller_state_t state = {.buttons = PLAYGATE_BUTTON_A, .left_x = 1234,
                                .left_y = -5678, .right_x = -9012,
                                .right_y = 23456};
    uint8_t first[SWITCH2_INPUT_REPORT_SIZE];
    uint8_t second[SWITCH2_INPUT_REPORT_SIZE];
    memset(first, 0xa5, sizeof(first));
    memset(second, 0x5a, sizeof(second));
    switch2_protocol_encode_input(11, 0x20, &state, first);
    switch2_protocol_encode_input(11, 0x20, &state, second);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(first, second, sizeof(first));
    TEST_ASSERT_EQUAL_HEX8(0, first[4] & 0xfc);
    for (size_t i = 0x0f; i < sizeof(first); ++i) {
        TEST_ASSERT_EQUAL_HEX8(0, first[i]);
    }
}
