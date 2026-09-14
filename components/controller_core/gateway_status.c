#include "gateway_status.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "controller_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

typedef struct {
    gateway_hid_state_t hid_state;
    bool wifi_connected;
    bool ble_connected;
    gateway_status_changed_cb_t callback;
    void *callback_context;
} gateway_status_state_t;

void gateway_status_notify_core_changed(void);

static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static gateway_status_state_t s_status;

static void notify_changed(void)
{
    gateway_status_changed_cb_t callback;
    void *context;

    portENTER_CRITICAL(&s_status_lock);
    callback = s_status.callback;
    context = s_status.callback_context;
    portEXIT_CRITICAL(&s_status_lock);

    if (callback != NULL) {
        callback(context);
    }
}

static void encode_u16_le(uint16_t value, uint8_t *out)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void encode_u32_le(uint32_t value, uint8_t *out)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static uint16_t saturate_u16(uint32_t value)
{
    return value > UINT16_MAX ? UINT16_MAX : (uint16_t)value;
}

void gateway_status_init(void)
{
    portENTER_CRITICAL(&s_status_lock);
    memset(&s_status, 0, sizeof(s_status));
    portEXIT_CRITICAL(&s_status_lock);
}

void gateway_status_set_hid_state(gateway_hid_state_t state)
{
    bool changed;

    portENTER_CRITICAL(&s_status_lock);
    changed = s_status.hid_state != state;
    s_status.hid_state = state;
    portEXIT_CRITICAL(&s_status_lock);

    if (changed) {
        notify_changed();
    }
}

void gateway_status_set_wifi_connected(bool connected)
{
    bool changed;

    portENTER_CRITICAL(&s_status_lock);
    changed = s_status.wifi_connected != connected;
    s_status.wifi_connected = connected;
    portEXIT_CRITICAL(&s_status_lock);

    if (changed) {
        notify_changed();
    }
}

void gateway_status_set_ble_connected(bool connected)
{
    bool changed;

    portENTER_CRITICAL(&s_status_lock);
    changed = s_status.ble_connected != connected;
    s_status.ble_connected = connected;
    portEXIT_CRITICAL(&s_status_lock);

    if (changed) {
        notify_changed();
    }
}

void gateway_status_set_changed_callback(gateway_status_changed_cb_t callback,
                                         void *context)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.callback = callback;
    s_status.callback_context = context;
    portEXIT_CRITICAL(&s_status_lock);
}

void gateway_status_notify_core_changed(void)
{
    notify_changed();
}

void gateway_status_get_view(gateway_status_view_t *out)
{
    if (out == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_status_lock);
    out->hid_state = s_status.hid_state;
    out->wifi_connected = s_status.wifi_connected;
    out->ble_connected = s_status.ble_connected;
    portEXIT_CRITICAL(&s_status_lock);
}

void gateway_status_encode(int64_t now_us, uint8_t out[GATEWAY_STATUS_SIZE])
{
    if (out == NULL) {
        return;
    }

    gateway_status_view_t view;
    gateway_status_get_view(&view);

    input_metrics_t metrics;
    input_core_snapshot(now_us, NULL, &metrics);

    memset(out, 0, GATEWAY_STATUS_SIZE);
    out[0] = GATEWAY_STATUS_VERSION;
    out[1] = (uint8_t)view.hid_state;
    out[2] = (uint8_t)metrics.active_source;
    out[3] = (view.wifi_connected ? UINT8_C(1) : UINT8_C(0)) |
             (view.ble_connected ? UINT8_C(2) : UINT8_C(0)) |
             (metrics.timeout_neutral ? UINT8_C(4) : UINT8_C(0));
    encode_u32_le(metrics.last_accepted_sequence, &out[4]);
    encode_u16_le(saturate_u16(metrics.accepted_total), &out[8]);
    encode_u16_le(saturate_u16(metrics.invalid), &out[10]);
    encode_u16_le(saturate_u16(metrics.stale), &out[12]);
    encode_u16_le(saturate_u16(metrics.conflicts), &out[14]);
    encode_u16_le(saturate_u16(metrics.superseded), &out[16]);
    encode_u16_le(saturate_u16(metrics.last_receive_to_submit_us), &out[18]);
}
