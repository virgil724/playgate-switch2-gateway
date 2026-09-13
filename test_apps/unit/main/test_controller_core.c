#include <stdint.h>
#include <string.h>

#include "controller_core.h"
#include "gateway_status.h"
#include "unity.h"

static void encode_u32_le(uint32_t value, uint8_t *out)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static void encode_i16_le(int16_t value, uint8_t *out)
{
    const uint16_t bits = (uint16_t)value;
    out[0] = (uint8_t)bits;
    out[1] = (uint8_t)(bits >> 8);
}

static void make_payload(uint8_t payload[PLAYGATE_INPUT_PAYLOAD_SIZE],
                         uint32_t buttons,
                         int16_t left_x,
                         int16_t left_y,
                         int16_t right_x,
                         int16_t right_y,
                         uint32_t sequence)
{
    payload[0] = PLAYGATE_INPUT_VERSION;
    encode_u32_le(buttons, &payload[1]);
    encode_i16_le(left_x, &payload[5]);
    encode_i16_le(left_y, &payload[7]);
    encode_i16_le(right_x, &payload[9]);
    encode_i16_le(right_y, &payload[11]);
    encode_u32_le(sequence, &payload[13]);
}

TEST_CASE("PlayGate golden payload decodes exactly", "[controller_core]")
{
    static const uint8_t golden[PLAYGATE_INPUT_PAYLOAD_SIZE] = {
        0x02, 0x01, 0x40, 0x00, 0x00, 0xff, 0x7f, 0x01, 0x80,
        0x00, 0x00, 0x00, 0x40, 0x04, 0x03, 0x02, 0x01,
    };

    input_core_init();
    TEST_ASSERT_EQUAL(INPUT_SUBMIT_ACCEPTED,
                      input_core_submit(INPUT_SOURCE_SERIAL, golden, sizeof(golden), 1234));

    controller_state_t state;
    input_metrics_t metrics;
    input_core_snapshot(1234, &state, &metrics);

    TEST_ASSERT_EQUAL_HEX32(PLAYGATE_BUTTON_A | PLAYGATE_BUTTON_DPAD_UP, state.buttons);
    TEST_ASSERT_EQUAL_INT16(32767, state.left_x);
    TEST_ASSERT_EQUAL_INT16(-32767, state.left_y);
    TEST_ASSERT_EQUAL_INT16(0, state.right_x);
    TEST_ASSERT_EQUAL_INT16(16384, state.right_y);
    TEST_ASSERT_EQUAL_HEX32(0x01020304, state.sequence);
    TEST_ASSERT_EQUAL_INT64(1234, state.received_at_us);
    TEST_ASSERT_EQUAL(INPUT_SOURCE_SERIAL, state.source);
    TEST_ASSERT_EQUAL_UINT64(1, state.generation);
    TEST_ASSERT_EQUAL_UINT32(1, metrics.accepted_total);
    TEST_ASSERT_EQUAL(INPUT_SOURCE_SERIAL, metrics.active_source);
}

TEST_CASE("invalid length and version cannot change controller state", "[controller_core]")
{
    uint8_t payload[PLAYGATE_INPUT_PAYLOAD_SIZE];
    make_payload(payload, PLAYGATE_BUTTON_A, 1, 2, 3, 4, 1);

    input_core_init();
    TEST_ASSERT_EQUAL(INPUT_SUBMIT_INVALID,
                      input_core_submit(INPUT_SOURCE_SERIAL, payload, sizeof(payload) - 1, 10));
    payload[0] = PLAYGATE_INPUT_VERSION + 1;
    TEST_ASSERT_EQUAL(INPUT_SUBMIT_INVALID,
                      input_core_submit(INPUT_SOURCE_SERIAL, payload, sizeof(payload), 20));

    controller_state_t state;
    input_metrics_t metrics;
    input_core_snapshot(20, &state, &metrics);
    TEST_ASSERT_EQUAL_UINT64(0, state.generation);
    TEST_ASSERT_EQUAL_UINT32(0, state.buttons);
    TEST_ASSERT_EQUAL(INPUT_SOURCE_NONE, state.source);
    TEST_ASSERT_EQUAL_UINT32(0, metrics.accepted_total);
    TEST_ASSERT_EQUAL_UINT32(2, metrics.invalid);
}

TEST_CASE("reserved buttons are masked and negative full scale is normalized", "[controller_core]")
{
    uint8_t payload[PLAYGATE_INPUT_PAYLOAD_SIZE];
    make_payload(payload, UINT32_MAX, INT16_MIN, INT16_MIN, INT16_MIN, INT16_MIN, 7);

    input_core_init();
    TEST_ASSERT_EQUAL(INPUT_SUBMIT_ACCEPTED,
                      input_core_submit(INPUT_SOURCE_UDP, payload, sizeof(payload), 0));

    controller_state_t state;
    input_core_snapshot(0, &state, NULL);
    TEST_ASSERT_EQUAL_HEX32(PLAYGATE_BUTTON_MASK, state.buttons);
    TEST_ASSERT_EQUAL_INT16(PLAYGATE_AXIS_MIN, state.left_x);
    TEST_ASSERT_EQUAL_INT16(PLAYGATE_AXIS_MIN, state.left_y);
    TEST_ASSERT_EQUAL_INT16(PLAYGATE_AXIS_MIN, state.right_x);
    TEST_ASSERT_EQUAL_INT16(PLAYGATE_AXIS_MIN, state.right_y);
}

TEST_CASE("sequence epochs reject duplicates and older values but accept wrap", "[controller_core]")
{
    uint8_t payload[PLAYGATE_INPUT_PAYLOAD_SIZE];
    input_core_init();

    make_payload(payload, 0, 0, 0, 0, 0, UINT32_MAX - 1);
    TEST_ASSERT_EQUAL(INPUT_SUBMIT_ACCEPTED,
                      input_core_submit(INPUT_SOURCE_SERIAL, payload, sizeof(payload), 0));
    TEST_ASSERT_EQUAL(INPUT_SUBMIT_STALE,
                      input_core_submit(INPUT_SOURCE_SERIAL, payload, sizeof(payload), 1));

    make_payload(payload, 0, 0, 0, 0, 0, UINT32_MAX - 2);
    TEST_ASSERT_EQUAL(INPUT_SUBMIT_STALE,
                      input_core_submit(INPUT_SOURCE_SERIAL, payload, sizeof(payload), 2));

    make_payload(payload, 0, 0, 0, 0, 0, 1);
    TEST_ASSERT_EQUAL(INPUT_SUBMIT_ACCEPTED,
                      input_core_submit(INPUT_SOURCE_SERIAL, payload, sizeof(payload), 3));

    controller_state_t state;
    input_metrics_t metrics;
    input_core_snapshot(3, &state, &metrics);
    TEST_ASSERT_EQUAL_UINT32(1, state.sequence);
    TEST_ASSERT_EQUAL_UINT32(2, metrics.accepted_total);
    TEST_ASSERT_EQUAL_UINT32(2, metrics.stale);
}

TEST_CASE("a source sequence epoch resets after lease-length silence", "[controller_core]")
{
    uint8_t payload[PLAYGATE_INPUT_PAYLOAD_SIZE];
    input_core_init();

    make_payload(payload, 0, 0, 0, 0, 0, 100);
    TEST_ASSERT_EQUAL(INPUT_SUBMIT_ACCEPTED,
                      input_core_submit(INPUT_SOURCE_SERIAL, payload, sizeof(payload), 0));
    make_payload(payload, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL(INPUT_SUBMIT_ACCEPTED,
                      input_core_submit(INPUT_SOURCE_SERIAL, payload, sizeof(payload), 300000));

    controller_state_t state;
    input_core_snapshot(300000, &state, NULL);
    TEST_ASSERT_EQUAL_UINT32(0, state.sequence);
}

TEST_CASE("owner lease records conflicts then hands over to a newer competitor", "[controller_core]")
{
    uint8_t payload[PLAYGATE_INPUT_PAYLOAD_SIZE];
    input_core_init();

    make_payload(payload, PLAYGATE_BUTTON_A, 0, 0, 0, 0, 1);
    TEST_ASSERT_EQUAL(INPUT_SUBMIT_ACCEPTED,
                      input_core_submit(INPUT_SOURCE_SERIAL, payload, sizeof(payload), 0));
    make_payload(payload, PLAYGATE_BUTTON_B, 0, 0, 0, 0, 10);
    TEST_ASSERT_EQUAL(INPUT_SUBMIT_CONFLICT,
                      input_core_submit(INPUT_SOURCE_UDP, payload, sizeof(payload), 100000));
    make_payload(payload, PLAYGATE_BUTTON_X, 0, 0, 0, 0, 11);
    TEST_ASSERT_EQUAL(INPUT_SUBMIT_ACCEPTED,
                      input_core_submit(INPUT_SOURCE_UDP, payload, sizeof(payload), 300000));

    controller_state_t state;
    input_metrics_t metrics;
    input_core_snapshot(300000, &state, &metrics);
    TEST_ASSERT_EQUAL(INPUT_SOURCE_UDP, state.source);
    TEST_ASSERT_EQUAL_HEX32(PLAYGATE_BUTTON_X, state.buttons);
    TEST_ASSERT_EQUAL_UINT32(11, state.sequence);
    TEST_ASSERT_EQUAL_UINT32(1, metrics.conflicts);
    TEST_ASSERT_EQUAL_UINT32(2, metrics.accepted_total);
}

TEST_CASE("snapshot neutralizes timed out controls without releasing ownership", "[controller_core]")
{
    uint8_t payload[PLAYGATE_INPUT_PAYLOAD_SIZE];
    make_payload(payload, PLAYGATE_BUTTON_HOME, 100, -200, 300, -400, 1);
    input_core_init();
    TEST_ASSERT_EQUAL(INPUT_SUBMIT_ACCEPTED,
                      input_core_submit(INPUT_SOURCE_BLE, payload, sizeof(payload), 1000));

    controller_state_t state;
    input_metrics_t metrics;
    input_core_snapshot(100999, &state, &metrics);
    TEST_ASSERT_EQUAL_HEX32(PLAYGATE_BUTTON_HOME, state.buttons);
    TEST_ASSERT_FALSE(metrics.timeout_neutral);

    input_core_snapshot(101000, &state, &metrics);
    TEST_ASSERT_EQUAL_UINT32(0, state.buttons);
    TEST_ASSERT_EQUAL_INT16(0, state.left_x);
    TEST_ASSERT_EQUAL_INT16(0, state.left_y);
    TEST_ASSERT_EQUAL_INT16(0, state.right_x);
    TEST_ASSERT_EQUAL_INT16(0, state.right_y);
    TEST_ASSERT_EQUAL(INPUT_SOURCE_BLE, state.source);
    TEST_ASSERT_EQUAL(INPUT_SOURCE_BLE, metrics.active_source);
    TEST_ASSERT_TRUE(metrics.timeout_neutral);
}

TEST_CASE("disconnect immediately releases and neutralizes its owner", "[controller_core]")
{
    uint8_t payload[PLAYGATE_INPUT_PAYLOAD_SIZE];
    make_payload(payload, PLAYGATE_BUTTON_ZR, 1, 2, 3, 4, 55);
    input_core_init();
    TEST_ASSERT_EQUAL(INPUT_SUBMIT_ACCEPTED,
                      input_core_submit(INPUT_SOURCE_BLE, payload, sizeof(payload), 10));

    input_core_release(INPUT_SOURCE_BLE);

    controller_state_t state;
    input_metrics_t metrics;
    input_core_snapshot(10, &state, &metrics);
    TEST_ASSERT_EQUAL_UINT32(0, state.buttons);
    TEST_ASSERT_EQUAL(INPUT_SOURCE_NONE, state.source);
    TEST_ASSERT_EQUAL(INPUT_SOURCE_NONE, metrics.active_source);
    TEST_ASSERT_FALSE(metrics.timeout_neutral);

    make_payload(payload, PLAYGATE_BUTTON_A, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL(INPUT_SUBMIT_ACCEPTED,
                      input_core_submit(INPUT_SOURCE_BLE, payload, sizeof(payload), 11));
}

TEST_CASE("superseded counts only unreported generations", "[controller_core]")
{
    uint8_t payload[PLAYGATE_INPUT_PAYLOAD_SIZE];
    input_core_init();

    make_payload(payload, 0, 0, 0, 0, 0, 1);
    TEST_ASSERT_EQUAL(INPUT_SUBMIT_ACCEPTED,
                      input_core_submit(INPUT_SOURCE_SERIAL, payload, sizeof(payload), 100));
    make_payload(payload, 0, 0, 0, 0, 0, 2);
    TEST_ASSERT_EQUAL(INPUT_SUBMIT_ACCEPTED,
                      input_core_submit(INPUT_SOURCE_SERIAL, payload, sizeof(payload), 200));

    controller_state_t state;
    input_metrics_t metrics;
    input_core_snapshot(200, &state, &metrics);
    TEST_ASSERT_EQUAL_UINT32(1, metrics.superseded);
    input_core_mark_reported(state.generation, 350);

    make_payload(payload, 0, 0, 0, 0, 0, 3);
    TEST_ASSERT_EQUAL(INPUT_SUBMIT_ACCEPTED,
                      input_core_submit(INPUT_SOURCE_SERIAL, payload, sizeof(payload), 400));
    input_core_snapshot(400, &state, &metrics);
    TEST_ASSERT_EQUAL_UINT32(1, metrics.superseded);
    TEST_ASSERT_EQUAL_UINT32(150, metrics.last_receive_to_submit_us);
}

typedef struct {
    unsigned calls;
    input_source_t source_seen;
} callback_probe_t;

static void status_changed_probe(void *context)
{
    callback_probe_t *probe = context;
    input_metrics_t metrics;
    input_core_snapshot(0, NULL, &metrics);
    ++probe->calls;
    probe->source_seen = metrics.active_source;
}

TEST_CASE("status callbacks run on changes and outside the core lock", "[controller_core]")
{
    uint8_t payload[PLAYGATE_INPUT_PAYLOAD_SIZE];
    callback_probe_t probe = {0};
    input_core_init();
    gateway_status_init();
    gateway_status_set_changed_callback(status_changed_probe, &probe);

    gateway_status_set_wifi_connected(false);
    TEST_ASSERT_EQUAL_UINT32(0, probe.calls);
    gateway_status_set_wifi_connected(true);
    TEST_ASSERT_EQUAL_UINT32(1, probe.calls);

    make_payload(payload, 0, 0, 0, 0, 0, 1);
    TEST_ASSERT_EQUAL(INPUT_SUBMIT_ACCEPTED,
                      input_core_submit(INPUT_SOURCE_SERIAL, payload, sizeof(payload), 0));
    TEST_ASSERT_EQUAL_UINT32(2, probe.calls);
    TEST_ASSERT_EQUAL(INPUT_SOURCE_SERIAL, probe.source_seen);

    input_core_release(INPUT_SOURCE_SERIAL);
    TEST_ASSERT_EQUAL_UINT32(3, probe.calls);
    TEST_ASSERT_EQUAL(INPUT_SOURCE_NONE, probe.source_seen);
}

TEST_CASE("gateway status uses exact layout and saturates every narrow counter", "[controller_core]")
{
    uint8_t payload[PLAYGATE_INPUT_PAYLOAD_SIZE];
    input_core_init();
    gateway_status_init();

    make_payload(payload, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL(INPUT_SUBMIT_ACCEPTED,
                      input_core_submit(INPUT_SOURCE_SERIAL, payload, sizeof(payload), 0));
    for (uint32_t sequence = 1; sequence <= UINT16_MAX + UINT32_C(1); ++sequence) {
        encode_u32_le(sequence, &payload[13]);
        (void)input_core_submit(INPUT_SOURCE_SERIAL, payload, sizeof(payload), 0);
    }

    controller_state_t state;
    input_core_snapshot(0, &state, NULL);
    input_core_mark_reported(state.generation, UINT16_MAX + INT64_C(100));

    for (uint32_t count = 0; count <= UINT16_MAX; ++count) {
        (void)input_core_submit(INPUT_SOURCE_SERIAL, payload, sizeof(payload), 0);
    }
    for (uint32_t sequence = 0; sequence <= UINT16_MAX; ++sequence) {
        encode_u32_le(sequence, &payload[13]);
        (void)input_core_submit(INPUT_SOURCE_UDP, payload, sizeof(payload), 0);
    }
    for (uint32_t count = 0; count <= UINT16_MAX; ++count) {
        (void)input_core_submit(INPUT_SOURCE_BLE, payload, sizeof(payload) - 1, 0);
    }

    gateway_status_set_hid_state(GATEWAY_HID_CONNECTED);
    gateway_status_set_wifi_connected(true);
    gateway_status_set_ble_connected(true);

    uint8_t status[GATEWAY_STATUS_SIZE];
    gateway_status_encode(0, status);
    static const uint8_t expected[GATEWAY_STATUS_SIZE] = {
        0x01, 0x03, 0x01, 0x03,
        0x00, 0x00, 0x01, 0x00,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, status, sizeof(expected));
}
