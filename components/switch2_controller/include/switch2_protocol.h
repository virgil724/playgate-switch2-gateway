#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controller_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH2_INPUT_REPORT_SIZE 63u
#define SWITCH2_MANUFACTURER_DATA_SIZE 26u
#define SWITCH2_PAIRING_RECORD_SIZE 38u
#define SWITCH2_COMMAND_RESPONSE_MAX_SIZE 128u
#define SWITCH2_COMMAND_VIBRATION_PREFIX_SIZE 33u
#define SWITCH2_CONSOLE_VALUE_HANDLE_COUNT 17u

typedef enum {
    SWITCH2_ADV_STANDARD = 0,
    SWITCH2_ADV_RECONNECT,
    SWITCH2_ADV_WAKE,
} switch2_advertising_mode_t;

typedef enum {
    SWITCH2_COMMAND_BASIC = 0,
    SWITCH2_COMMAND_COMBINED,
} switch2_command_endpoint_t;

typedef enum {
    SWITCH2_COMMAND_OK = 0,
    SWITCH2_COMMAND_MALFORMED,
    SWITCH2_COMMAND_RESPONSE_TOO_SMALL,
    SWITCH2_COMMAND_STATE_REJECTED,
    SWITCH2_COMMAND_CRYPTO_ERROR,
} switch2_command_result_t;

typedef struct {
    uint8_t controller_address[6];
    uint8_t console_address[6];
    uint8_t ltk[16];
    bool paired;
} switch2_pairing_record_t;

typedef struct {
    bool feature_activation;
    bool clear_pairing;
    bool pairing_finalized;
    bool direct_pairing_received;
    uint8_t feature_mask;
    uint8_t console_address[6];
    uint8_t ltk[16];
} switch2_command_effects_t;

typedef struct {
    uint64_t generation;
    uint8_t controller_address_reversed[6];
    uint8_t console_address[6];
    uint8_t ltk[16];
    uint8_t feature_mask;
    uint8_t pairing_stage;
} switch2_protocol_t;

extern const uint16_t
    switch2_console_value_handles[SWITCH2_CONSOLE_VALUE_HANDLE_COUNT];

void switch2_protocol_init(switch2_protocol_t *protocol,
                           const uint8_t controller_address[6]);
void switch2_protocol_begin_connection(switch2_protocol_t *protocol,
                                       uint64_t generation);
void switch2_protocol_encode_input(uint8_t counter,
                                   uint8_t feature_mask,
                                   const controller_state_t *state,
                                   uint8_t out[SWITCH2_INPUT_REPORT_SIZE]);
void switch2_protocol_encode_manufacturer_data(
    switch2_advertising_mode_t mode,
    const uint8_t console_address[6],
    uint8_t out[SWITCH2_MANUFACTURER_DATA_SIZE]);
void switch2_protocol_derive_ltk(const uint8_t a1[16], uint8_t out_ltk[16]);
bool switch2_protocol_pairing_challenge(const uint8_t ltk[16],
                                        const uint8_t a2[16],
                                        uint8_t out_b2[16]);
void switch2_protocol_pairing_record_encode(
    const switch2_pairing_record_t *record,
    uint8_t out[SWITCH2_PAIRING_RECORD_SIZE]);
bool switch2_protocol_pairing_record_decode(
    const uint8_t *bytes,
    size_t len,
    switch2_pairing_record_t *out);
switch2_command_result_t switch2_protocol_process_command(
    switch2_protocol_t *protocol,
    uint64_t generation,
    switch2_command_endpoint_t endpoint,
    const uint8_t *request,
    size_t request_len,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len,
    switch2_command_effects_t *effects);

#ifdef __cplusplus
}
#endif
