#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GATEWAY_STATUS_VERSION 1u
#define GATEWAY_STATUS_SIZE 20u

typedef enum {
    GATEWAY_HID_IDLE = 0,
    GATEWAY_HID_DISCOVERABLE = 1,
    GATEWAY_HID_CONNECTING = 2,
    GATEWAY_HID_CONNECTED = 3,
} gateway_hid_state_t;

typedef struct {
    gateway_hid_state_t hid_state;
    bool wifi_connected;
    bool ble_connected;
} gateway_status_view_t;

typedef void (*gateway_status_changed_cb_t)(void *context);

void gateway_status_init(void);
void gateway_status_set_hid_state(gateway_hid_state_t state);
void gateway_status_set_wifi_connected(bool connected);
void gateway_status_set_ble_connected(bool connected);
void gateway_status_set_changed_callback(gateway_status_changed_cb_t callback,
                                         void *context);
void gateway_status_get_view(gateway_status_view_t *out);
void gateway_status_encode(int64_t now_us, uint8_t out[GATEWAY_STATUS_SIZE]);

#ifdef __cplusplus
}
#endif
