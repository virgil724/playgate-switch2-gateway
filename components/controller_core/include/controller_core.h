#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLAYGATE_INPUT_VERSION 2u
#define PLAYGATE_INPUT_PAYLOAD_SIZE 17u
#define PLAYGATE_BUTTON_MASK UINT32_C(0x0003FFFF)
#define PLAYGATE_AXIS_MIN (-32767)
#define PLAYGATE_AXIS_MAX 32767

typedef enum {
    INPUT_SOURCE_NONE = 0,
    INPUT_SOURCE_SERIAL = 1,
    INPUT_SOURCE_UDP = 2,
    INPUT_SOURCE_BLE = 3,
    INPUT_SOURCE_COUNT
} input_source_t;

typedef enum {
    INPUT_SUBMIT_ACCEPTED = 0,
    INPUT_SUBMIT_INVALID,
    INPUT_SUBMIT_STALE,
    INPUT_SUBMIT_CONFLICT
} input_submit_result_t;

typedef enum {
    PLAYGATE_BUTTON_A = UINT32_C(1) << 0,
    PLAYGATE_BUTTON_B = UINT32_C(1) << 1,
    PLAYGATE_BUTTON_X = UINT32_C(1) << 2,
    PLAYGATE_BUTTON_Y = UINT32_C(1) << 3,
    PLAYGATE_BUTTON_L = UINT32_C(1) << 4,
    PLAYGATE_BUTTON_R = UINT32_C(1) << 5,
    PLAYGATE_BUTTON_ZL = UINT32_C(1) << 6,
    PLAYGATE_BUTTON_ZR = UINT32_C(1) << 7,
    PLAYGATE_BUTTON_PLUS = UINT32_C(1) << 8,
    PLAYGATE_BUTTON_MINUS = UINT32_C(1) << 9,
    PLAYGATE_BUTTON_HOME = UINT32_C(1) << 10,
    PLAYGATE_BUTTON_CAPTURE = UINT32_C(1) << 11,
    PLAYGATE_BUTTON_L_STICK = UINT32_C(1) << 12,
    PLAYGATE_BUTTON_R_STICK = UINT32_C(1) << 13,
    PLAYGATE_BUTTON_DPAD_UP = UINT32_C(1) << 14,
    PLAYGATE_BUTTON_DPAD_DOWN = UINT32_C(1) << 15,
    PLAYGATE_BUTTON_DPAD_LEFT = UINT32_C(1) << 16,
    PLAYGATE_BUTTON_DPAD_RIGHT = UINT32_C(1) << 17,
} playgate_button_t;

typedef struct {
    uint32_t buttons;
    int16_t left_x;
    int16_t left_y;
    int16_t right_x;
    int16_t right_y;
    uint32_t sequence;
    int64_t received_at_us;
    input_source_t source;
    uint64_t generation;
} controller_state_t;

typedef struct {
    uint32_t accepted_total;
    uint32_t invalid;
    uint32_t stale;
    uint32_t conflicts;
    uint32_t superseded;
    uint32_t last_receive_to_submit_us;
    input_source_t active_source;
    uint32_t last_accepted_sequence;
    bool timeout_neutral;
} input_metrics_t;

typedef void (*input_core_updated_cb_t)(void *context);

void input_core_init(void);
void input_core_set_updated_callback(input_core_updated_cb_t callback,
                                     void *context);
input_submit_result_t input_core_submit(input_source_t source,
                                        const uint8_t *bytes,
                                        size_t len,
                                        int64_t now_us);
void input_core_release(input_source_t source);
void input_core_snapshot(int64_t now_us,
                         controller_state_t *out_state,
                         input_metrics_t *out_metrics);
void input_core_mark_reported(uint64_t generation, int64_t submitted_at_us);

#ifdef __cplusplus
}
#endif
