#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_store.h"
#include "switch2_controller.h"
#include "switch2_protocol.h"

#define SWITCH2_CONSOLE_ADV_INSTANCE 0u
#define SWITCH2_PLAYGATE_ADV_INSTANCE 1u
#define SWITCH2_NO_CONN BLE_HS_CONN_HANDLE_NONE
#define SWITCH2_INPUT_VALUE_HANDLE 0x000eu
#define SWITCH2_BASIC_COMMAND_VALUE_HANDLE 0x0014u
#define SWITCH2_COMBINED_COMMAND_VALUE_HANDLE 0x0016u
#define SWITCH2_BASIC_RESPONSE_VALUE_HANDLE 0x001au
#define SWITCH2_COMBINED_RESPONSE_VALUE_HANDLE 0x001eu

typedef struct {
    switch2_pairing_record_t record;
    struct ble_store_value_sec security;
} switch2_pairing_store_t;

typedef struct {
    uint16_t console_conn_handle;
    uint16_t playgate_conn_handle;
    uint64_t console_generation;
    uint64_t playgate_generation;
    bool console_encrypted;
    bool console_input_subscribed;
    bool console_feature_active;
    bool console_ready;
    bool playgate_status_subscribed;
    bool waiting_for_playgate_disconnect;
    uint8_t report_counter;
    uint8_t feature_mask;
    uint8_t status[20];
    switch2_controller_metrics_t metrics;
} switch2_runtime_t;

extern portMUX_TYPE g_switch2_lock;
extern switch2_runtime_t g_switch2_runtime;
extern switch2_protocol_t g_switch2_protocol;
extern switch2_pairing_store_t g_switch2_pairing;
extern uint16_t g_switch2_playgate_input_handle;
extern uint16_t g_switch2_playgate_status_handle;

int switch2_gatt_register(void);
bool switch2_gatt_handles_valid(void);
void switch2_gatt_register_callback(struct ble_gatt_register_ctxt *ctxt,
                                    void *arg);
int switch2_gap_event(struct ble_gap_event *event, void *arg);
int switch2_start_console_advertising(bool wake);
int switch2_start_playgate_advertising(void);
void switch2_stop_playgate_advertising(void);
void switch2_runtime_feature_activated(uint8_t feature_mask);
void switch2_runtime_pairing_cleared(void);

esp_err_t switch2_pairing_load_or_create(switch2_pairing_store_t *store);
esp_err_t switch2_pairing_commit(switch2_pairing_store_t *store,
                                 const uint8_t console_address[6],
                                 const uint8_t ltk[16]);
esp_err_t switch2_pairing_clear(switch2_pairing_store_t *store);
int switch2_pairing_inject(const switch2_pairing_store_t *store);
void switch2_pairing_delete_injected(const switch2_pairing_store_t *store);

void switch2_metric_notify_error(void);
void switch2_metric_mbuf_unavailable(void);
