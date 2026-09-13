#include "controller_core.h"

#include <limits.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "sdkconfig.h"

#ifndef CONFIG_PLAYGATE_INPUT_TIMEOUT_MS
#define CONFIG_PLAYGATE_INPUT_TIMEOUT_MS 100
#endif

#ifndef CONFIG_PLAYGATE_SOURCE_LEASE_MS
#define CONFIG_PLAYGATE_SOURCE_LEASE_MS 300
#endif

#define INPUT_TIMEOUT_US (INT64_C(1000) * CONFIG_PLAYGATE_INPUT_TIMEOUT_MS)
#define SOURCE_LEASE_US (INT64_C(1000) * CONFIG_PLAYGATE_SOURCE_LEASE_MS)

typedef struct {
    bool valid;
    uint32_t sequence;
    int64_t last_seen_at_us;
} source_epoch_t;

/* Internal component hook; deliberately not part of the public status API. */
void gateway_status_notify_core_changed(void);

static portMUX_TYPE s_core_lock = portMUX_INITIALIZER_UNLOCKED;
static controller_state_t s_state;
static input_metrics_t s_metrics;
static source_epoch_t s_epochs[INPUT_SOURCE_COUNT];
static int64_t s_owner_lease_at_us;
static uint64_t s_generation;
static bool s_current_generation_reported;
static uint64_t s_sampled_generation;
static int64_t s_sampled_received_at_us;
static bool s_sampled_has_receive_timestamp;

static void increment_saturating(uint32_t *value)
{
    if (*value != UINT32_MAX) {
        ++(*value);
    }
}

static uint32_t decode_u32_le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static int16_t decode_axis_le(const uint8_t *bytes)
{
    const uint16_t raw = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
    int32_t value = raw;

    if (raw > INT16_MAX) {
        value -= UINT16_MAX + 1L;
    }
    if (value < PLAYGATE_AXIS_MIN) {
        value = PLAYGATE_AXIS_MIN;
    }
    return (int16_t)value;
}

static bool decode_payload(input_source_t source,
                           const uint8_t *bytes,
                           size_t len,
                           int64_t now_us,
                           controller_state_t *decoded)
{
    if (source <= INPUT_SOURCE_NONE || source >= INPUT_SOURCE_COUNT ||
        bytes == NULL || len != PLAYGATE_INPUT_PAYLOAD_SIZE ||
        bytes[0] != PLAYGATE_INPUT_VERSION) {
        return false;
    }

    decoded->buttons = decode_u32_le(&bytes[1]) & PLAYGATE_BUTTON_MASK;
    decoded->left_x = decode_axis_le(&bytes[5]);
    decoded->left_y = decode_axis_le(&bytes[7]);
    decoded->right_x = decode_axis_le(&bytes[9]);
    decoded->right_y = decode_axis_le(&bytes[11]);
    decoded->sequence = decode_u32_le(&bytes[13]);
    decoded->received_at_us = now_us;
    decoded->source = source;
    decoded->generation = 0;
    return true;
}

static bool interval_elapsed(int64_t now_us, int64_t then_us, int64_t interval_us)
{
    return now_us >= then_us &&
           (uint64_t)now_us - (uint64_t)then_us >= (uint64_t)interval_us;
}

static bool sequence_is_newer(uint32_t sequence, uint32_t previous)
{
    return (int32_t)(sequence - previous) > 0;
}

void input_core_init(void)
{
    portENTER_CRITICAL(&s_core_lock);
    memset(&s_state, 0, sizeof(s_state));
    memset(&s_metrics, 0, sizeof(s_metrics));
    memset(s_epochs, 0, sizeof(s_epochs));
    s_owner_lease_at_us = 0;
    s_generation = 0;
    s_current_generation_reported = true;
    s_sampled_generation = 0;
    s_sampled_received_at_us = 0;
    s_sampled_has_receive_timestamp = false;
    portEXIT_CRITICAL(&s_core_lock);
}

input_submit_result_t input_core_submit(input_source_t source,
                                        const uint8_t *bytes,
                                        size_t len,
                                        int64_t now_us)
{
    controller_state_t decoded;
    if (!decode_payload(source, bytes, len, now_us, &decoded)) {
        portENTER_CRITICAL(&s_core_lock);
        increment_saturating(&s_metrics.invalid);
        portEXIT_CRITICAL(&s_core_lock);
        return INPUT_SUBMIT_INVALID;
    }

    input_submit_result_t result;
    bool notify_status = false;

    portENTER_CRITICAL(&s_core_lock);

    source_epoch_t *epoch = &s_epochs[source];
    if (epoch->valid && interval_elapsed(now_us, epoch->last_seen_at_us, SOURCE_LEASE_US)) {
        epoch->valid = false;
    }

    if (epoch->valid && !sequence_is_newer(decoded.sequence, epoch->sequence)) {
        epoch->last_seen_at_us = now_us;
        increment_saturating(&s_metrics.stale);
        result = INPUT_SUBMIT_STALE;
    } else {
        epoch->valid = true;
        epoch->sequence = decoded.sequence;
        epoch->last_seen_at_us = now_us;

        const input_source_t owner = s_metrics.active_source;
        const bool owner_lease_expired =
            owner != INPUT_SOURCE_NONE &&
            interval_elapsed(now_us, s_owner_lease_at_us, SOURCE_LEASE_US);

        if (owner != INPUT_SOURCE_NONE && owner != source && !owner_lease_expired) {
            increment_saturating(&s_metrics.conflicts);
            result = INPUT_SUBMIT_CONFLICT;
        } else {
            if (s_generation != 0 && !s_current_generation_reported) {
                increment_saturating(&s_metrics.superseded);
            }

            const input_source_t previous_owner = owner;
            const bool was_timeout_neutral = s_metrics.timeout_neutral;
            decoded.generation = ++s_generation;
            s_state = decoded;
            s_current_generation_reported = false;
            s_owner_lease_at_us = now_us;

            increment_saturating(&s_metrics.accepted_total);
            s_metrics.active_source = source;
            s_metrics.last_accepted_sequence = decoded.sequence;
            s_metrics.timeout_neutral = false;
            notify_status = previous_owner != source || was_timeout_neutral;
            result = INPUT_SUBMIT_ACCEPTED;
        }
    }

    portEXIT_CRITICAL(&s_core_lock);

    if (notify_status) {
        gateway_status_notify_core_changed();
    }
    return result;
}

void input_core_release(input_source_t source)
{
    if (source <= INPUT_SOURCE_NONE || source >= INPUT_SOURCE_COUNT) {
        return;
    }

    bool notify_status = false;

    portENTER_CRITICAL(&s_core_lock);
    memset(&s_epochs[source], 0, sizeof(s_epochs[source]));

    if (s_metrics.active_source == source) {
        memset(&s_state, 0, sizeof(s_state));
        s_state.generation = ++s_generation;
        s_current_generation_reported = false;
        s_owner_lease_at_us = 0;
        s_metrics.active_source = INPUT_SOURCE_NONE;
        s_metrics.timeout_neutral = false;
        notify_status = true;
    }
    portEXIT_CRITICAL(&s_core_lock);

    if (notify_status) {
        gateway_status_notify_core_changed();
    }
}

void input_core_snapshot(int64_t now_us,
                         controller_state_t *out_state,
                         input_metrics_t *out_metrics)
{
    bool notify_status = false;

    portENTER_CRITICAL(&s_core_lock);

    const bool timeout_neutral =
        s_metrics.active_source != INPUT_SOURCE_NONE &&
        interval_elapsed(now_us, s_state.received_at_us, INPUT_TIMEOUT_US);
    if (timeout_neutral != s_metrics.timeout_neutral) {
        s_metrics.timeout_neutral = timeout_neutral;
        notify_status = true;
    }

    if (out_state != NULL) {
        *out_state = s_state;
        if (timeout_neutral) {
            out_state->buttons = 0;
            out_state->left_x = 0;
            out_state->left_y = 0;
            out_state->right_x = 0;
            out_state->right_y = 0;
        }
        s_sampled_generation = s_state.generation;
        s_sampled_received_at_us = s_state.received_at_us;
        s_sampled_has_receive_timestamp = s_state.source != INPUT_SOURCE_NONE;
    }
    if (out_metrics != NULL) {
        *out_metrics = s_metrics;
    }

    portEXIT_CRITICAL(&s_core_lock);

    if (notify_status) {
        gateway_status_notify_core_changed();
    }
}

void input_core_mark_reported(uint64_t generation, int64_t submitted_at_us)
{
    portENTER_CRITICAL(&s_core_lock);

    if (generation == s_state.generation) {
        s_current_generation_reported = true;
    }

    if (generation != 0 && generation == s_sampled_generation &&
        s_sampled_has_receive_timestamp) {
        uint64_t latency_us = 0;
        if (submitted_at_us >= s_sampled_received_at_us) {
            latency_us = (uint64_t)(submitted_at_us - s_sampled_received_at_us);
        }
        s_metrics.last_receive_to_submit_us =
            latency_us > UINT32_MAX ? UINT32_MAX : (uint32_t)latency_us;
    }

    portEXIT_CRITICAL(&s_core_lock);
}
