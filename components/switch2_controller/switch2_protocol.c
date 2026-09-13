/*
 * Protocol facts and fixtures are from ndeadly/switch2_controller_research.
 * Tables and behavior are substantially ported from
 * https://github.com/zhantss/ESP32-BLE5-NSController-Emulator at
 * 0ea0c62aeab2440c7f9c77f29cf87ffa3b4c192f (MIT; see bundled notice).
 */
#include "switch2_protocol.h"

#include <limits.h>
#include <string.h>

#include "psa/crypto.h"

#define SWITCH2_RECORD_MAGIC UINT32_C(0x47503253) /* "S2PG" little-endian */
#define SWITCH2_RECORD_VERSION 1u
#define SWITCH2_RESPONSE_PREFIX_SIZE 14u
#define SWITCH2_COMMAND_HEADER_SIZE 8u
#define SWITCH2_BLUETOOTH_MEMORY_MAX 0x4fu
#define SWITCH2_PAIRING_STAGE_ADDRESS 1u
#define SWITCH2_PAIRING_STAGE_KEY 2u
#define SWITCH2_PAIRING_STAGE_CHALLENGE 3u
#define SWITCH2_PAIRING_STAGE_FINAL 4u

static const uint8_t s_pairing_b1[16] = {
    0x5c, 0xf6, 0xee, 0x79, 0x2c, 0xdf, 0x05, 0xe1,
    0xba, 0x2b, 0x63, 0x25, 0xc4, 0x1a, 0x5f, 0x10,
};

static const uint8_t s_device_info[] = {
    0x01, 0x00,
    0x48, 0x45, 0x4a, 0x37, 0x31, 0x30, 0x30, 0x31,
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x00, 0x00,
    0x7e, 0x05, 0x69, 0x20, 0x01, 0x06, 0x01,
    0x23, 0x23, 0x23, 0x63, 0xb9, 0x7a,
    0xe6, 0xe6, 0xe6, 0x32, 0x32, 0x32,
};
static const uint8_t s_memory_013040[] = {
    0x3b, 0xe0, 0xd3, 0x41, 0xc6, 0x60, 0x6a, 0xbc,
    0x4d, 0xd7, 0xa2, 0xbb, 0x71, 0x1e, 0xdd, 0x37,
};
static const uint8_t s_memory_013080[] = {
    0x01, 0xad, 0xd9, 0x9a, 0x55, 0x56, 0x65, 0xa0, 0x00, 0x0a,
    0xa0, 0x00, 0x0a, 0xe2, 0x20, 0x0e, 0xe2, 0x20, 0x0e, 0x9a,
    0xad, 0xd9, 0x9a, 0xad, 0xd9, 0x0a, 0xa5, 0x50, 0x0a, 0xa5,
    0x50, 0x2f, 0xf6, 0x62, 0x2f, 0xf6, 0x62, 0x0a, 0xff, 0xff,
};
static const uint8_t s_memory_0130a8[] = {
    0xb3, 0x67, 0x83, 0x2e, 0x66, 0x5e, 0x3a, 0x06, 0x5f,
};
static const uint8_t s_memory_0130e8[] = {
    0x2c, 0x08, 0x84, 0xd1, 0x65, 0x63, 0x2a, 0x26, 0x62,
};
static const uint8_t s_memory_013100[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xa6, 0xf2, 0x62, 0xbd,
    0xa8, 0x00, 0x08, 0x3d, 0x2f, 0xed, 0x20, 0x41,
};

typedef struct {
    uint32_t address;
    const uint8_t *bytes;
    size_t len;
} memory_block_t;

static const memory_block_t s_memory[] = {
    {UINT32_C(0x013000), s_device_info, sizeof(s_device_info)},
    {UINT32_C(0x013040), s_memory_013040, sizeof(s_memory_013040)},
    {UINT32_C(0x013080), s_memory_013080, sizeof(s_memory_013080)},
    {UINT32_C(0x0130a8), s_memory_0130a8, sizeof(s_memory_0130a8)},
    {UINT32_C(0x0130c0), s_memory_013080, sizeof(s_memory_013080)},
    {UINT32_C(0x0130e8), s_memory_0130e8, sizeof(s_memory_0130e8)},
    {UINT32_C(0x013100), s_memory_013100, sizeof(s_memory_013100)},
};

const uint16_t switch2_console_value_handles[SWITCH2_CONSOLE_VALUE_HANDLE_COUNT] = {
    0x0003, 0x0005, 0x0007, 0x000a, 0x000e, 0x0012,
    0x0014, 0x0016, 0x0018, 0x001a, 0x001e, 0x0022,
    0x0026, 0x002a, 0x002c, 0x002e, 0x0032,
};

static uint32_t read_le32(const uint8_t bytes[4])
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void write_le32(uint8_t bytes[4], uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t crc32_ieee(const uint8_t *bytes, size_t len)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < len; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return ~crc;
}

static bool bytes_nonzero(const uint8_t *bytes, size_t len)
{
    uint8_t combined = 0;
    for (size_t i = 0; i < len; ++i) {
        combined |= bytes[i];
    }
    return combined != 0;
}

void switch2_protocol_init(switch2_protocol_t *protocol,
                           const uint8_t controller_address[6])
{
    if (protocol == NULL) {
        return;
    }
    memset(protocol, 0, sizeof(*protocol));
    if (controller_address != NULL) {
        for (size_t i = 0; i < 6; ++i) {
            protocol->controller_address_reversed[i] = controller_address[5 - i];
        }
    }
}

void switch2_protocol_begin_connection(switch2_protocol_t *protocol,
                                       uint64_t generation)
{
    if (protocol == NULL) {
        return;
    }
    protocol->generation = generation;
    memset(protocol->console_address, 0, sizeof(protocol->console_address));
    memset(protocol->ltk, 0, sizeof(protocol->ltk));
    protocol->feature_mask = 0;
    protocol->pairing_stage = 0;
}

static int32_t clamp_axis(int32_t value)
{
    if (value < PLAYGATE_AXIS_MIN) {
        return PLAYGATE_AXIS_MIN;
    }
    if (value > PLAYGATE_AXIS_MAX) {
        return PLAYGATE_AXIS_MAX;
    }
    return value;
}

static uint16_t scale_axis(int32_t value)
{
    value = clamp_axis(value);
    if (value < 0) {
        return (uint16_t)(2048 + ((int64_t)value * 2048) / PLAYGATE_AXIS_MAX);
    }
    return (uint16_t)(2048 + ((int64_t)value * 2047) / PLAYGATE_AXIS_MAX);
}

static void pack_stick(uint16_t x, uint16_t y, uint8_t out[3])
{
    out[0] = (uint8_t)x;
    out[1] = (uint8_t)((x >> 8) | ((y & 0x0f) << 4));
    out[2] = (uint8_t)(y >> 4);
}

void switch2_protocol_encode_input(uint8_t counter,
                                   uint8_t feature_mask,
                                   const controller_state_t *state,
                                   uint8_t out[SWITCH2_INPUT_REPORT_SIZE])
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, SWITCH2_INPUT_REPORT_SIZE);
    out[0] = counter;
    out[1] = 0x20;
    out[0x0b] = (feature_mask & 0x20u) != 0 ? 0x38 : 0x30;
    out[0x0e] = 0x28;
    if (state == NULL) {
        pack_stick(0x800, 0x800, &out[5]);
        pack_stick(0x800, 0x800, &out[8]);
        return;
    }

    const uint32_t b = state->buttons;
    out[2] = ((b & PLAYGATE_BUTTON_B) ? 0x01 : 0) |
             ((b & PLAYGATE_BUTTON_A) ? 0x02 : 0) |
             ((b & PLAYGATE_BUTTON_Y) ? 0x04 : 0) |
             ((b & PLAYGATE_BUTTON_X) ? 0x08 : 0) |
             ((b & PLAYGATE_BUTTON_R) ? 0x10 : 0) |
             ((b & PLAYGATE_BUTTON_ZR) ? 0x20 : 0) |
             ((b & PLAYGATE_BUTTON_PLUS) ? 0x40 : 0) |
             ((b & PLAYGATE_BUTTON_R_STICK) ? 0x80 : 0);
    out[3] = ((b & PLAYGATE_BUTTON_DPAD_DOWN) ? 0x01 : 0) |
             ((b & PLAYGATE_BUTTON_DPAD_RIGHT) ? 0x02 : 0) |
             ((b & PLAYGATE_BUTTON_DPAD_LEFT) ? 0x04 : 0) |
             ((b & PLAYGATE_BUTTON_DPAD_UP) ? 0x08 : 0) |
             ((b & PLAYGATE_BUTTON_L) ? 0x10 : 0) |
             ((b & PLAYGATE_BUTTON_ZL) ? 0x20 : 0) |
             ((b & PLAYGATE_BUTTON_MINUS) ? 0x40 : 0) |
             ((b & PLAYGATE_BUTTON_L_STICK) ? 0x80 : 0);
    out[4] = ((b & PLAYGATE_BUTTON_HOME) ? 0x01 : 0) |
             ((b & PLAYGATE_BUTTON_CAPTURE) ? 0x02 : 0);

    const int32_t ly = -clamp_axis(state->left_y);
    const int32_t ry = -clamp_axis(state->right_y);
    pack_stick(scale_axis(state->left_x), scale_axis(ly), &out[5]);
    pack_stick(scale_axis(state->right_x), scale_axis(ry), &out[8]);
}

void switch2_protocol_encode_manufacturer_data(
    switch2_advertising_mode_t mode,
    const uint8_t console_address[6],
    uint8_t out[SWITCH2_MANUFACTURER_DATA_SIZE])
{
    static const uint8_t base[SWITCH2_MANUFACTURER_DATA_SIZE] = {
        0x53, 0x05, 0x01, 0x00, 0x03, 0x7e, 0x05, 0x69, 0x20,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    if (out == NULL) {
        return;
    }
    memcpy(out, base, sizeof(base));
    if (mode == SWITCH2_ADV_WAKE) {
        out[11] = 0x81;
    }
    if (mode != SWITCH2_ADV_STANDARD && console_address != NULL) {
        memcpy(&out[12], console_address, 6);
    }
}

void switch2_protocol_derive_ltk(const uint8_t a1[16], uint8_t out_ltk[16])
{
    if (a1 == NULL || out_ltk == NULL) {
        return;
    }
    for (size_t i = 0; i < 16; ++i) {
        out_ltk[i] = a1[i] ^ s_pairing_b1[i];
    }
}

bool switch2_protocol_pairing_challenge(const uint8_t ltk[16],
                                        const uint8_t a2[16],
                                        uint8_t out_b2[16])
{
    if (ltk == NULL || a2 == NULL || out_b2 == NULL) {
        return false;
    }

    uint8_t key_reversed[16];
    uint8_t challenge_reversed[16];
    for (size_t i = 0; i < 16; ++i) {
        key_reversed[i] = ltk[15 - i];
        challenge_reversed[i] = a2[15 - i];
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key = 0;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECB_NO_PADDING);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_import_key(&attributes, key_reversed, sizeof(key_reversed), &key) != PSA_SUCCESS) {
        psa_reset_key_attributes(&attributes);
        return false;
    }

    size_t output_len = 0;
    const psa_status_t status = psa_cipher_encrypt(
        key, PSA_ALG_ECB_NO_PADDING, challenge_reversed,
        sizeof(challenge_reversed), out_b2, 16, &output_len);
    (void)psa_destroy_key(key);
    psa_reset_key_attributes(&attributes);
    return status == PSA_SUCCESS && output_len == 16;
}

void switch2_protocol_pairing_record_encode(
    const switch2_pairing_record_t *record,
    uint8_t out[SWITCH2_PAIRING_RECORD_SIZE])
{
    if (record == NULL || out == NULL) {
        return;
    }
    memset(out, 0, SWITCH2_PAIRING_RECORD_SIZE);
    write_le32(&out[0], SWITCH2_RECORD_MAGIC);
    out[4] = SWITCH2_RECORD_VERSION;
    memcpy(&out[5], record->controller_address, 6);
    out[11] = record->paired ? 1 : 0;
    if (record->paired) {
        memcpy(&out[12], record->console_address, 6);
        memcpy(&out[18], record->ltk, 16);
    }
    write_le32(&out[34], crc32_ieee(out, 34));
}

bool switch2_protocol_pairing_record_decode(
    const uint8_t *bytes,
    size_t len,
    switch2_pairing_record_t *out)
{
    if (bytes == NULL || out == NULL || len != SWITCH2_PAIRING_RECORD_SIZE ||
        read_le32(bytes) != SWITCH2_RECORD_MAGIC ||
        bytes[4] != SWITCH2_RECORD_VERSION ||
        read_le32(&bytes[34]) != crc32_ieee(bytes, 34) ||
        bytes[5] != 0x78 || bytes[6] != 0x81 || bytes[7] != 0x8c ||
        bytes[11] > 1) {
        return false;
    }
    if (bytes[11] == 1 &&
        (!bytes_nonzero(&bytes[12], 6) || !bytes_nonzero(&bytes[18], 16))) {
        return false;
    }
    if (bytes[11] == 0 &&
        (bytes_nonzero(&bytes[12], 6) || bytes_nonzero(&bytes[18], 16))) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    memcpy(out->controller_address, &bytes[5], 6);
    out->paired = bytes[11] == 1;
    if (out->paired) {
        memcpy(out->console_address, &bytes[12], 6);
        memcpy(out->ltk, &bytes[18], 16);
    }
    return true;
}

static bool exact_len(size_t actual, size_t expected)
{
    return actual == expected;
}

static void encode_feature_info(uint8_t flags, uint8_t out[8])
{
    memset(out, 0, 8);
    out[0] = (flags & 0x01u) ? 0x07 : 0;
    out[1] = (flags & 0x02u) ? 0x07 : 0;
    out[2] = (flags & 0x04u) ? 0x01 : 0;
    out[3] = (flags & 0x80u) ? 0x01 : 0;
    out[4] = (flags & 0x10u) ? 0x01 : 0;
    out[5] = (flags & 0x20u) ? 0x03 : 0;
}

static void simulated_memory_read(uint32_t address, size_t len, uint8_t *out)
{
    memset(out, 0xff, len);
    const uint64_t request_end = (uint64_t)address + len;
    for (size_t i = 0; i < sizeof(s_memory) / sizeof(s_memory[0]); ++i) {
        const uint64_t block_start = s_memory[i].address;
        const uint64_t block_end = block_start + s_memory[i].len;
        if (request_end <= block_start || block_end <= address) {
            continue;
        }
        const uint64_t start = address > block_start ? address : block_start;
        const uint64_t end = request_end < block_end ? request_end : block_end;
        memcpy(&out[start - address], &s_memory[i].bytes[start - block_start],
               (size_t)(end - start));
    }
}

static switch2_command_result_t dispatch_command(
    switch2_protocol_t *protocol,
    uint64_t generation,
    uint8_t command,
    uint8_t subcommand,
    const uint8_t *data,
    size_t data_len,
    uint8_t *out,
    size_t *out_len,
    switch2_command_effects_t *effects)
{
    *out_len = 0;
    switch (command) {
    case 0x01:
        if (subcommand != 0x0c || !exact_len(data_len, 0)) {
            return subcommand == 0x0c ? SWITCH2_COMMAND_MALFORMED : SWITCH2_COMMAND_OK;
        }
        memcpy(out, (const uint8_t[]){0x61, 0x12, 0x50, 0x10}, 4);
        *out_len = 4;
        return SWITCH2_COMMAND_OK;

    case 0x02:
        if (subcommand != 0x04) {
            return SWITCH2_COMMAND_OK;
        }
        if (!exact_len(data_len, 8) || data[0] == 0 ||
            data[0] > SWITCH2_BLUETOOTH_MEMORY_MAX) {
            return SWITCH2_COMMAND_MALFORMED;
        }
        {
            const uint32_t address = read_le32(&data[4]);
            const size_t read_len = data[0];
            if ((uint64_t)address + read_len > (uint64_t)UINT32_MAX + 1u) {
                return SWITCH2_COMMAND_MALFORMED;
            }
            memcpy(out, data, 8);
            out[1] = 0;
            simulated_memory_read(address, read_len, &out[8]);
            *out_len = 8 + read_len;
        }
        return SWITCH2_COMMAND_OK;

    case 0x03:
        if (subcommand == 0x07) {
            if (!exact_len(data_len, 22) || !bytes_nonzero(data, 6) ||
                !bytes_nonzero(&data[6], 16)) {
                return SWITCH2_COMMAND_MALFORMED;
            }
            effects->direct_pairing_received = true;
            memcpy(effects->console_address, data, 6);
            memcpy(effects->ltk, &data[6], 16);
            return SWITCH2_COMMAND_OK;
        }
        if (subcommand == 0x08) {
            if (!exact_len(data_len, 0)) {
                return SWITCH2_COMMAND_MALFORMED;
            }
            effects->clear_pairing = true;
            return SWITCH2_COMMAND_OK;
        }
        return SWITCH2_COMMAND_OK;

    case 0x07:
        if (subcommand != 0x01 || !exact_len(data_len, 0)) {
            return subcommand == 0x01 ? SWITCH2_COMMAND_MALFORMED : SWITCH2_COMMAND_OK;
        }
        out[0] = 0;
        *out_len = 1;
        return SWITCH2_COMMAND_OK;

    case 0x0c:
        if (subcommand < 0x01 || subcommand > 0x05) {
            return SWITCH2_COMMAND_OK;
        }
        if (!exact_len(data_len, 4)) {
            return SWITCH2_COMMAND_MALFORMED;
        }
        memset(out, 0, 12);
        if (subcommand == 0x01) {
            encode_feature_info(data[0], &out[4]);
            *out_len = 12;
        } else {
            *out_len = 4;
            if (subcommand == 0x02) {
                protocol->feature_mask = data[0];
            } else if (subcommand == 0x03) {
                protocol->feature_mask = 0;
            } else if (subcommand == 0x04) {
                protocol->feature_mask |= data[0];
                effects->feature_activation = true;
                effects->feature_mask = protocol->feature_mask;
            } else {
                protocol->feature_mask &= (uint8_t)~data[0];
                effects->feature_mask = protocol->feature_mask;
            }
        }
        return SWITCH2_COMMAND_OK;

    case 0x10:
        if (subcommand != 0x01 || !exact_len(data_len, 0)) {
            return subcommand == 0x01 ? SWITCH2_COMMAND_MALFORMED : SWITCH2_COMMAND_OK;
        }
        memcpy(out, (const uint8_t[]){
                   0x02, 0x01, 0x04, 0x02, 0x0c, 0x00,
                   0x00, 0x00, 0x00, 0x02, 0x03, 0x00}, 12);
        *out_len = 12;
        return SWITCH2_COMMAND_OK;

    case 0x11:
        if (!exact_len(data_len, 0)) {
            return SWITCH2_COMMAND_MALFORMED;
        }
        if (subcommand == 0x01) {
            memcpy(out, (const uint8_t[]){0x01, 0x00, 0x00, 0x00}, 4);
            *out_len = 4;
        } else if (subcommand == 0x03) {
            static const uint8_t response[] = {
                0x01, 0x20, 0x03, 0x00, 0x00, 0x0a, 0xe8, 0x1c,
                0x3b, 0x79, 0x7d, 0x8b, 0x3a, 0x0a, 0xe8, 0x9c,
                0x42, 0x58, 0xa0, 0x0b, 0x42, 0x0a, 0xe8, 0x9c,
                0x41, 0x58, 0xa0, 0x0b, 0x41,
            };
            memcpy(out, response, sizeof(response));
            *out_len = sizeof(response);
        }
        return SWITCH2_COMMAND_OK;

    case 0x15:
        if (generation != protocol->generation) {
            return SWITCH2_COMMAND_STATE_REJECTED;
        }
        if (subcommand == 0x01) {
            if (data_len != 0) {
                if (data_len < 8 || data[0] != 0 || data[1] == 0 ||
                    data_len != 2u + 6u * data[1]) {
                    return SWITCH2_COMMAND_MALFORMED;
                }
                memcpy(protocol->console_address, &data[2], 6);
                protocol->pairing_stage = SWITCH2_PAIRING_STAGE_ADDRESS;
            }
            memcpy(out, (const uint8_t[]){0x01, 0x04, 0x01}, 3);
            memcpy(&out[3], protocol->controller_address_reversed, 6);
            *out_len = 9;
            return SWITCH2_COMMAND_OK;
        }
        if (subcommand == 0x04) {
            if (!exact_len(data_len, 17) || data[0] != 0) {
                return SWITCH2_COMMAND_MALFORMED;
            }
            if (protocol->pairing_stage != SWITCH2_PAIRING_STAGE_ADDRESS) {
                return SWITCH2_COMMAND_STATE_REJECTED;
            }
            switch2_protocol_derive_ltk(&data[1], protocol->ltk);
            protocol->pairing_stage = SWITCH2_PAIRING_STAGE_KEY;
            out[0] = 0x01;
            memcpy(&out[1], s_pairing_b1, 16);
            *out_len = 17;
            return SWITCH2_COMMAND_OK;
        }
        if (subcommand == 0x02) {
            if (!exact_len(data_len, 17) || data[0] != 0) {
                return SWITCH2_COMMAND_MALFORMED;
            }
            if (protocol->pairing_stage != SWITCH2_PAIRING_STAGE_KEY) {
                return SWITCH2_COMMAND_STATE_REJECTED;
            }
            out[0] = 0x01;
            if (!switch2_protocol_pairing_challenge(protocol->ltk, &data[1], &out[1])) {
                return SWITCH2_COMMAND_CRYPTO_ERROR;
            }
            protocol->pairing_stage = SWITCH2_PAIRING_STAGE_CHALLENGE;
            *out_len = 17;
            return SWITCH2_COMMAND_OK;
        }
        if (subcommand == 0x03) {
            if (!exact_len(data_len, 1) || data[0] != 0) {
                return SWITCH2_COMMAND_MALFORMED;
            }
            if (protocol->pairing_stage != SWITCH2_PAIRING_STAGE_CHALLENGE ||
                !bytes_nonzero(protocol->console_address, 6) ||
                !bytes_nonzero(protocol->ltk, 16)) {
                return SWITCH2_COMMAND_STATE_REJECTED;
            }
            protocol->pairing_stage = SWITCH2_PAIRING_STAGE_FINAL;
            effects->pairing_finalized = true;
            memcpy(effects->console_address, protocol->console_address, 6);
            memcpy(effects->ltk, protocol->ltk, 16);
            out[0] = 0x01;
            *out_len = 1;
            return SWITCH2_COMMAND_OK;
        }
        return SWITCH2_COMMAND_OK;

    case 0x16:
        if (subcommand != 0x01 || !exact_len(data_len, 0)) {
            return subcommand == 0x01 ? SWITCH2_COMMAND_MALFORMED : SWITCH2_COMMAND_OK;
        }
        memset(out, 0, 0x18);
        *out_len = 0x18;
        return SWITCH2_COMMAND_OK;

    case 0x18:
        if (subcommand != 0x01 || !exact_len(data_len, 0)) {
            return subcommand == 0x01 ? SWITCH2_COMMAND_MALFORMED : SWITCH2_COMMAND_OK;
        }
        memcpy(out, (const uint8_t[]){0x00, 0x00, 0x40, 0xf0,
                                      0x00, 0x00, 0x60, 0x00}, 8);
        *out_len = 8;
        return SWITCH2_COMMAND_OK;

    case 0x09:
    case 0x0a:
        /* Upstream registers no handler for these and answers with an empty
         * payload via its default branch, ignoring the request payload. */
        return SWITCH2_COMMAND_OK;

    default:
        return SWITCH2_COMMAND_OK;
    }
}

switch2_command_result_t switch2_protocol_process_command(
    switch2_protocol_t *protocol,
    uint64_t generation,
    switch2_command_endpoint_t endpoint,
    const uint8_t *request,
    size_t request_len,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len,
    switch2_command_effects_t *effects)
{
    if (response_len != NULL) {
        *response_len = 0;
    }
    if (effects != NULL) {
        memset(effects, 0, sizeof(*effects));
    }
    if (protocol == NULL || request == NULL || response == NULL ||
        response_len == NULL || effects == NULL ||
        (endpoint != SWITCH2_COMMAND_BASIC && endpoint != SWITCH2_COMMAND_COMBINED)) {
        return SWITCH2_COMMAND_MALFORMED;
    }

    const size_t request_prefix = endpoint == SWITCH2_COMMAND_COMBINED
                                      ? SWITCH2_COMMAND_VIBRATION_PREFIX_SIZE
                                      : 0;
    const size_t response_prefix = endpoint == SWITCH2_COMMAND_COMBINED
                                       ? SWITCH2_RESPONSE_PREFIX_SIZE
                                       : 0;
    if (request_len < request_prefix + SWITCH2_COMMAND_HEADER_SIZE ||
        (request_prefix != 0 && request[0] != 0)) {
        return SWITCH2_COMMAND_MALFORMED;
    }
    const uint8_t *header = &request[request_prefix];
    const size_t data_len = request_len - request_prefix - SWITCH2_COMMAND_HEADER_SIZE;
    if (header[1] != 0x91 || header[2] != 0x01 || header[4] != 0 ||
        header[5] != data_len || header[6] != 0 || header[7] != 0) {
        return SWITCH2_COMMAND_MALFORMED;
    }

    uint8_t payload[SWITCH2_COMMAND_RESPONSE_MAX_SIZE - SWITCH2_COMMAND_HEADER_SIZE] = {0};
    size_t payload_len = 0;
    const switch2_command_result_t result = dispatch_command(
        protocol, generation, header[0], header[3],
        &header[SWITCH2_COMMAND_HEADER_SIZE], data_len,
        payload, &payload_len, effects);
    if (result != SWITCH2_COMMAND_OK) {
        memset(effects, 0, sizeof(*effects));
        return result;
    }

    const size_t total = response_prefix + SWITCH2_COMMAND_HEADER_SIZE + payload_len;
    if (total > response_capacity || total > SWITCH2_COMMAND_RESPONSE_MAX_SIZE) {
        memset(effects, 0, sizeof(*effects));
        return SWITCH2_COMMAND_RESPONSE_TOO_SMALL;
    }
    memset(response, 0, total);
    uint8_t *response_header = &response[response_prefix];
    response_header[0] = header[0];
    response_header[1] = 0x01;
    response_header[2] = 0x01;
    response_header[3] = header[3];
    response_header[4] = 0x10;
    response_header[5] = 0x78;
    if (payload_len != 0) {
        memcpy(&response_header[SWITCH2_COMMAND_HEADER_SIZE], payload, payload_len);
    }
    *response_len = total;
    return SWITCH2_COMMAND_OK;
}
