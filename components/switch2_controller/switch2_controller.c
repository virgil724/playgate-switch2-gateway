/*
 * Single-owner NimBLE lifecycle and Nintendo advertising behavior. Protocol
 * values derive from the public research and pinned MIT source named in the
 * bundled provenance notice.
 */
#include "switch2_internal.h"

#include <limits.h>
#include <string.h>

#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway_status.h"
#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "sdkconfig.h"

#ifndef CONFIG_PLAYGATE_REPORT_HZ
#define CONFIG_PLAYGATE_REPORT_HZ 100
#endif

#define SWITCH2_START_TIMEOUT_MS 5000u
#define SWITCH2_REPORT_STACK_WORDS 2048u
#define SWITCH2_ADV_INTERVAL_30_MS 0x30u
#define SWITCH2_ADV_ARG(instance) ((void *)(uintptr_t)((instance) + 1u))
#define SWITCH2_CONN_ARG_TAG UINT32_C(0x80000000)
#define SWITCH2_CONN_TOKEN_MASK UINT32_C(0x3fffffff)
#define SWITCH2_CONN_ARG(instance, generation) \
    ((void *)(uintptr_t)(SWITCH2_CONN_ARG_TAG | \
                         (((uint32_t)(generation) & SWITCH2_CONN_TOKEN_MASK) << 1) | \
                         (instance)))

portMUX_TYPE g_switch2_lock = portMUX_INITIALIZER_UNLOCKED;
switch2_runtime_t g_switch2_runtime;
switch2_protocol_t g_switch2_protocol;
switch2_pairing_store_t g_switch2_pairing;

static StaticTask_t s_report_tcb;
static StackType_t s_report_stack[SWITCH2_REPORT_STACK_WORDS];
static TaskHandle_t s_report_task;
static TaskHandle_t s_start_waiter;
static esp_err_t s_start_result;
static bool s_started;
static bool s_console_adv_configured;
static bool s_playgate_adv_configured;
static uint8_t s_report[SWITCH2_INPUT_REPORT_SIZE];

static void increment_saturating(uint32_t *value)
{
    if (*value != UINT32_MAX) {
        ++*value;
    }
}

void switch2_metric_notify_error(void)
{
    portENTER_CRITICAL(&g_switch2_lock);
    increment_saturating(&g_switch2_runtime.metrics.notify_errors);
    portEXIT_CRITICAL(&g_switch2_lock);
}

void switch2_metric_mbuf_unavailable(void)
{
    portENTER_CRITICAL(&g_switch2_lock);
    increment_saturating(&g_switch2_runtime.metrics.mbuf_unavailable);
    portEXIT_CRITICAL(&g_switch2_lock);
}

static int store_read(int object_type, const union ble_store_key *key,
                      union ble_store_value *value)
{
    if (key == NULL || value == NULL || !g_switch2_pairing.record.paired) {
        return BLE_HS_ENOENT;
    }
    if (object_type != BLE_STORE_OBJ_TYPE_OUR_SEC &&
        object_type != BLE_STORE_OBJ_TYPE_PEER_SEC) {
        return BLE_HS_ENOENT;
    }
    if (key->sec.idx != 0 ||
        (ble_addr_cmp(&key->sec.peer_addr, BLE_ADDR_ANY) != 0 &&
         ble_addr_cmp(&key->sec.peer_addr,
                      &g_switch2_pairing.security.peer_addr) != 0)) {
        return BLE_HS_ENOENT;
    }
    value->sec = g_switch2_pairing.security;
    return 0;
}

static int store_write(int object_type, const union ble_store_value *value)
{
    if (value == NULL) {
        return BLE_HS_EINVAL;
    }
    if (object_type == BLE_STORE_OBJ_TYPE_CCCD) {
        return 0;
    }
    if (object_type != BLE_STORE_OBJ_TYPE_OUR_SEC &&
        object_type != BLE_STORE_OBJ_TYPE_PEER_SEC) {
        return BLE_HS_ENOTSUP;
    }
    if (!g_switch2_pairing.record.paired ||
        ble_addr_cmp(&value->sec.peer_addr,
                     &g_switch2_pairing.security.peer_addr) != 0 ||
        value->sec.key_size != 16 || !value->sec.ltk_present ||
        memcmp(value->sec.ltk, g_switch2_pairing.security.ltk, 16) != 0) {
        return BLE_HS_EAUTHEN;
    }
    return 0;
}

static int store_delete(int object_type, const union ble_store_key *key)
{
    (void)object_type;
    (void)key;
    return 0;
}

static bool console_ready_transition(void)
{
    bool became_ready = false;
    bool lost_ready = false;
    uint16_t playgate = SWITCH2_NO_CONN;
    portENTER_CRITICAL(&g_switch2_lock);
    const bool ready = g_switch2_runtime.console_conn_handle != SWITCH2_NO_CONN &&
                       g_switch2_runtime.console_encrypted &&
                       g_switch2_runtime.console_input_subscribed &&
                       g_switch2_runtime.console_feature_active;
    if (ready && !g_switch2_runtime.console_ready) {
        g_switch2_runtime.console_ready = true;
        became_ready = true;
    } else if (!ready && g_switch2_runtime.console_ready) {
        g_switch2_runtime.console_ready = false;
        g_switch2_runtime.playgate_status_subscribed = false;
        playgate = g_switch2_runtime.playgate_conn_handle;
        lost_ready = true;
    }
    portEXIT_CRITICAL(&g_switch2_lock);
    if (became_ready) {
        gateway_status_set_hid_state(GATEWAY_HID_CONNECTED);
        (void)switch2_start_playgate_advertising();
    } else if (lost_ready) {
        switch2_stop_playgate_advertising();
        gateway_status_set_hid_state(GATEWAY_HID_CONNECTING);
        if (playgate != SWITCH2_NO_CONN) {
            input_core_release(INPUT_SOURCE_BLE);
            gateway_status_set_ble_connected(false);
            (void)ble_gap_terminate(playgate, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    return became_ready;
}

void switch2_runtime_feature_activated(uint8_t feature_mask)
{
    portENTER_CRITICAL(&g_switch2_lock);
    g_switch2_runtime.feature_mask = feature_mask;
    g_switch2_runtime.console_feature_active = true;
    portEXIT_CRITICAL(&g_switch2_lock);
    (void)console_ready_transition();
}

void switch2_runtime_pairing_cleared(void)
{
    uint16_t console;
    uint16_t playgate;
    switch2_stop_playgate_advertising();
    portENTER_CRITICAL(&g_switch2_lock);
    console = g_switch2_runtime.console_conn_handle;
    playgate = g_switch2_runtime.playgate_conn_handle;
    g_switch2_runtime.console_ready = false;
    g_switch2_runtime.console_feature_active = false;
    g_switch2_runtime.console_encrypted = false;
    g_switch2_runtime.console_input_subscribed = false;
    g_switch2_runtime.playgate_status_subscribed = false;
    portEXIT_CRITICAL(&g_switch2_lock);
    if (playgate != SWITCH2_NO_CONN) {
        input_core_release(INPUT_SOURCE_BLE);
        (void)ble_gap_terminate(playgate, BLE_ERR_REM_USER_CONN_TERM);
    }
    if (console != SWITCH2_NO_CONN) {
        (void)ble_gap_terminate(console, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static int configure_advertising(uint8_t instance)
{
    struct ble_gap_ext_adv_params params;
    memset(&params, 0, sizeof(params));
    params.legacy_pdu = 1;
    params.connectable = 1;
    params.scannable = 1;
    params.own_addr_type = BLE_OWN_ADDR_PUBLIC;
    params.primary_phy = BLE_HCI_LE_PHY_1M;
    params.secondary_phy = BLE_HCI_LE_PHY_1M;
    params.itvl_min = SWITCH2_ADV_INTERVAL_30_MS;
    params.itvl_max = SWITCH2_ADV_INTERVAL_30_MS;
    params.channel_map = BLE_GAP_ADV_DFLT_CHANNEL_MAP;
    params.filter_policy = BLE_HCI_SCAN_FILT_NO_WL;
    params.sid = instance;
    return ble_gap_ext_adv_configure(instance, &params, NULL,
                                     switch2_gap_event,
                                     SWITCH2_ADV_ARG(instance));
}

static int set_advertising_data(uint8_t instance,
                                const uint8_t *bytes, size_t len)
{
    struct os_mbuf *om = os_msys_get_pkthdr(len, 0);
    if (om == NULL) {
        return BLE_HS_ENOMEM;
    }
    if (os_mbuf_append(om, bytes, len) != 0) {
        os_mbuf_free_chain(om);
        return BLE_HS_ENOMEM;
    }
    return ble_gap_ext_adv_set_data(instance, om);
}

int switch2_start_console_advertising(bool wake)
{
    if (ble_gap_ext_adv_active(SWITCH2_CONSOLE_ADV_INSTANCE)) {
        return 0;
    }
    if (!s_console_adv_configured) {
        const int rc = configure_advertising(SWITCH2_CONSOLE_ADV_INSTANCE);
        if (rc != 0) {
            return rc;
        }
        s_console_adv_configured = true;
    }

    uint8_t manufacturer[SWITCH2_MANUFACTURER_DATA_SIZE];
    const switch2_advertising_mode_t mode =
        !g_switch2_pairing.record.paired
            ? SWITCH2_ADV_STANDARD
            : (wake ? SWITCH2_ADV_WAKE : SWITCH2_ADV_RECONNECT);
    switch2_protocol_encode_manufacturer_data(
        mode, g_switch2_pairing.record.console_address, manufacturer);
    uint8_t advertising[31] = {0x02, 0x01, 0x06, 0x1b, 0xff};
    memcpy(&advertising[5], manufacturer, sizeof(manufacturer));
    int rc = set_advertising_data(SWITCH2_CONSOLE_ADV_INSTANCE,
                                  advertising, sizeof(advertising));
    if (rc == 0) {
        rc = ble_gap_ext_adv_start(SWITCH2_CONSOLE_ADV_INSTANCE, 0, 0);
    }
    if (rc == 0) {
        gateway_status_set_hid_state(GATEWAY_HID_DISCOVERABLE);
    }
    return rc;
}

int switch2_start_playgate_advertising(void)
{
    bool ready;
    bool occupied;
    portENTER_CRITICAL(&g_switch2_lock);
    ready = g_switch2_runtime.console_ready;
    occupied = g_switch2_runtime.playgate_conn_handle != SWITCH2_NO_CONN;
    portEXIT_CRITICAL(&g_switch2_lock);
    if (!ready || occupied) {
        return BLE_HS_EBUSY;
    }
    if (ble_gap_ext_adv_active(SWITCH2_PLAYGATE_ADV_INSTANCE)) {
        return 0;
    }
    if (!s_playgate_adv_configured) {
        const int rc = configure_advertising(SWITCH2_PLAYGATE_ADV_INSTANCE);
        if (rc != 0) {
            return rc;
        }
        s_playgate_adv_configured = true;
    }

    static const uint8_t advertising[] = {
        0x02,0x01,0x06, 0x11,0x07,
        0xe1,0xf1,0xd7,0xa9,0x70,0x4c,0x7a,0x9b,
        0x93,0x4f,0x4d,0x3b,0x2e,0x9c,0x5c,0x5f,
    };
    static const uint8_t scan_response[] = {
        0x0f,0x09,'P','l','a','y','G','a','t','e',' ','I','n','p','u','t',
    };
    int rc = set_advertising_data(SWITCH2_PLAYGATE_ADV_INSTANCE,
                                  advertising, sizeof(advertising));
    if (rc == 0) {
        struct os_mbuf *om = os_msys_get_pkthdr(sizeof(scan_response), 0);
        if (om == NULL || os_mbuf_append(om, scan_response,
                                         sizeof(scan_response)) != 0) {
            if (om != NULL) {
                os_mbuf_free_chain(om);
            }
            rc = BLE_HS_ENOMEM;
        } else {
            rc = ble_gap_ext_adv_rsp_set_data(SWITCH2_PLAYGATE_ADV_INSTANCE, om);
        }
    }
    if (rc == 0) {
        rc = ble_gap_ext_adv_start(SWITCH2_PLAYGATE_ADV_INSTANCE, 0, 0);
    }
    return rc;
}

void switch2_stop_playgate_advertising(void)
{
    if (ble_gap_ext_adv_active(SWITCH2_PLAYGATE_ADV_INSTANCE)) {
        (void)ble_gap_ext_adv_stop(SWITCH2_PLAYGATE_ADV_INSTANCE);
    }
}

static void console_connected(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0 ||
        (g_switch2_pairing.record.paired &&
         (desc.peer_ota_addr.type != BLE_ADDR_PUBLIC ||
          memcmp(desc.peer_ota_addr.val,
                 g_switch2_pairing.record.console_address, 6) != 0))) {
        (void)ble_gap_terminate(conn_handle, BLE_ERR_AUTH_FAIL);
        return;
    }

    bool accept;
    uint64_t generation;
    portENTER_CRITICAL(&g_switch2_lock);
    accept = g_switch2_runtime.console_conn_handle == SWITCH2_NO_CONN;
    if (accept) {
        g_switch2_runtime.console_conn_handle = conn_handle;
        generation = ++g_switch2_runtime.console_generation;
        if (generation > 1) {
            increment_saturating(&g_switch2_runtime.metrics.console_reconnects);
        }
        g_switch2_runtime.console_encrypted = false;
        g_switch2_runtime.console_input_subscribed = false;
        g_switch2_runtime.console_feature_active = false;
        g_switch2_runtime.console_ready = false;
        g_switch2_runtime.feature_mask = 0;
        g_switch2_runtime.report_counter = 0;
    } else {
        generation = g_switch2_runtime.console_generation;
    }
    portEXIT_CRITICAL(&g_switch2_lock);
    if (!accept) {
        (void)ble_gap_terminate(conn_handle, BLE_ERR_CONN_LIMIT);
        return;
    }
    switch2_protocol_begin_connection(&g_switch2_protocol, generation);
    if (ble_gap_set_event_cb(conn_handle, switch2_gap_event,
                             SWITCH2_CONN_ARG(SWITCH2_CONSOLE_ADV_INSTANCE,
                                              generation)) != 0) {
        (void)ble_gap_terminate(conn_handle, BLE_ERR_UNSPECIFIED);
        return;
    }
    gateway_status_set_hid_state(GATEWAY_HID_CONNECTING);
}

static void playgate_connected(uint16_t conn_handle)
{
    bool accept;
    uint64_t generation;
    portENTER_CRITICAL(&g_switch2_lock);
    accept = g_switch2_runtime.console_ready &&
             g_switch2_runtime.playgate_conn_handle == SWITCH2_NO_CONN;
    if (accept) {
        g_switch2_runtime.playgate_conn_handle = conn_handle;
        generation = ++g_switch2_runtime.playgate_generation;
        g_switch2_runtime.playgate_status_subscribed = false;
    } else {
        generation = g_switch2_runtime.playgate_generation;
    }
    portEXIT_CRITICAL(&g_switch2_lock);
    if (!accept) {
        (void)ble_gap_terminate(conn_handle, BLE_ERR_CONN_LIMIT);
        return;
    }
    if (ble_gap_set_event_cb(conn_handle, switch2_gap_event,
                             SWITCH2_CONN_ARG(SWITCH2_PLAYGATE_ADV_INSTANCE,
                                              generation)) != 0) {
        (void)ble_gap_terminate(conn_handle, BLE_ERR_UNSPECIFIED);
        return;
    }
    gateway_status_set_ble_connected(true);
}

static bool generation_matches(uint64_t current, uint32_t token,
                               bool token_valid)
{
    return !token_valid ||
           ((uint32_t)current & SWITCH2_CONN_TOKEN_MASK) == token;
}

static bool role_event_matches(uint8_t instance, uint16_t conn_handle,
                               uint32_t token, bool token_valid)
{
    bool matched;
    portENTER_CRITICAL(&g_switch2_lock);
    if (instance == SWITCH2_CONSOLE_ADV_INSTANCE) {
        matched = conn_handle == g_switch2_runtime.console_conn_handle &&
                  generation_matches(g_switch2_runtime.console_generation,
                                     token, token_valid);
    } else {
        matched = conn_handle == g_switch2_runtime.playgate_conn_handle &&
                  generation_matches(g_switch2_runtime.playgate_generation,
                                     token, token_valid);
    }
    portEXIT_CRITICAL(&g_switch2_lock);
    return matched;
}

static void console_disconnected(uint16_t conn_handle, uint32_t token,
                                 bool token_valid)
{
    uint16_t playgate = SWITCH2_NO_CONN;
    bool matched;
    portENTER_CRITICAL(&g_switch2_lock);
    matched = conn_handle == g_switch2_runtime.console_conn_handle &&
              generation_matches(g_switch2_runtime.console_generation,
                                 token, token_valid);
    if (matched) {
        g_switch2_runtime.console_conn_handle = SWITCH2_NO_CONN;
        g_switch2_runtime.console_encrypted = false;
        g_switch2_runtime.console_input_subscribed = false;
        g_switch2_runtime.console_feature_active = false;
        g_switch2_runtime.console_ready = false;
        g_switch2_runtime.feature_mask = 0;
        playgate = g_switch2_runtime.playgate_conn_handle;
        g_switch2_runtime.playgate_status_subscribed = false;
        g_switch2_runtime.waiting_for_playgate_disconnect = playgate != SWITCH2_NO_CONN;
    }
    portEXIT_CRITICAL(&g_switch2_lock);
    if (!matched) {
        return;
    }
    switch2_stop_playgate_advertising();
    gateway_status_set_hid_state(GATEWAY_HID_IDLE);
    if (playgate != SWITCH2_NO_CONN) {
        input_core_release(INPUT_SOURCE_BLE);
        gateway_status_set_ble_connected(false);
        (void)ble_gap_terminate(playgate, BLE_ERR_REM_USER_CONN_TERM);
    } else {
        (void)switch2_start_console_advertising(g_switch2_pairing.record.paired);
    }
}

static void playgate_disconnected(uint16_t conn_handle, uint32_t token,
                                  bool token_valid)
{
    bool matched;
    bool console_waiting;
    bool console_ready;
    portENTER_CRITICAL(&g_switch2_lock);
    matched = conn_handle == g_switch2_runtime.playgate_conn_handle &&
              generation_matches(g_switch2_runtime.playgate_generation,
                                 token, token_valid);
    if (matched) {
        g_switch2_runtime.playgate_conn_handle = SWITCH2_NO_CONN;
        g_switch2_runtime.playgate_status_subscribed = false;
    }
    console_waiting = matched && g_switch2_runtime.waiting_for_playgate_disconnect;
    if (console_waiting) {
        g_switch2_runtime.waiting_for_playgate_disconnect = false;
    }
    console_ready = g_switch2_runtime.console_ready;
    portEXIT_CRITICAL(&g_switch2_lock);
    if (!matched) {
        return;
    }
    input_core_release(INPUT_SOURCE_BLE);
    gateway_status_set_ble_connected(false);
    if (console_waiting) {
        (void)switch2_start_console_advertising(g_switch2_pairing.record.paired);
    } else if (console_ready) {
        (void)switch2_start_playgate_advertising();
    }
}

int switch2_gap_event(struct ble_gap_event *event, void *arg)
{
    if (event == NULL || (uintptr_t)arg == 0) {
        return 0;
    }
    const uint32_t encoded = (uint32_t)(uintptr_t)arg;
    const bool token_valid = (encoded & SWITCH2_CONN_ARG_TAG) != 0;
    const uint8_t instance = token_valid
                                 ? (uint8_t)(encoded & 1u)
                                 : (uint8_t)(encoded - 1u);
    const uint32_t token = token_valid
                               ? ((encoded >> 1) & SWITCH2_CONN_TOKEN_MASK)
                               : 0;
    if (instance > SWITCH2_PLAYGATE_ADV_INSTANCE) {
        if (event->type == BLE_GAP_EVENT_CONNECT && event->connect.status == 0) {
            (void)ble_gap_terminate(event->connect.conn_handle, BLE_ERR_CONN_LIMIT);
        }
        return 0;
    }

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            if (instance == SWITCH2_CONSOLE_ADV_INSTANCE) {
                (void)switch2_start_console_advertising(g_switch2_pairing.record.paired);
            }
            return 0;
        }
        if (instance == SWITCH2_CONSOLE_ADV_INSTANCE) {
            console_connected(event->connect.conn_handle);
        } else {
            playgate_connected(event->connect.conn_handle);
        }
        return 0;


    case BLE_GAP_EVENT_DISCONNECT:
        if (instance == SWITCH2_CONSOLE_ADV_INSTANCE) {
            console_disconnected(event->disconnect.conn.conn_handle,
                                 token, token_valid);
        } else {
            playgate_disconnected(event->disconnect.conn.conn_handle,
                                  token, token_valid);
        }
        return 0;


    case BLE_GAP_EVENT_ENC_CHANGE:
        if (instance == SWITCH2_CONSOLE_ADV_INSTANCE) {
            struct ble_gap_conn_desc desc;
            bool matched;
            const bool encrypted = event->enc_change.status == 0 &&
                ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0 &&
                desc.sec_state.encrypted;
            portENTER_CRITICAL(&g_switch2_lock);
            matched = event->enc_change.conn_handle ==
                          g_switch2_runtime.console_conn_handle &&
                      generation_matches(g_switch2_runtime.console_generation,
                                         token, token_valid);
            if (matched) {
                g_switch2_runtime.console_encrypted = encrypted;
            }
            portEXIT_CRITICAL(&g_switch2_lock);
            if (matched) {
                (void)console_ready_transition();
            }
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (instance == SWITCH2_CONSOLE_ADV_INSTANCE &&
            event->subscribe.attr_handle == SWITCH2_INPUT_VALUE_HANDLE) {
            bool matched;
            portENTER_CRITICAL(&g_switch2_lock);
            matched = event->subscribe.conn_handle ==
                          g_switch2_runtime.console_conn_handle &&
                      generation_matches(g_switch2_runtime.console_generation,
                                         token, token_valid);
            if (matched) {
                g_switch2_runtime.console_input_subscribed =
                    event->subscribe.cur_notify != 0;
            }
            portEXIT_CRITICAL(&g_switch2_lock);
            if (matched) {
                (void)console_ready_transition();
            }
        } else if (instance == SWITCH2_PLAYGATE_ADV_INSTANCE &&
                   event->subscribe.attr_handle ==
                       g_switch2_playgate_status_handle) {
            portENTER_CRITICAL(&g_switch2_lock);
            if (event->subscribe.conn_handle ==
                    g_switch2_runtime.playgate_conn_handle &&
                generation_matches(g_switch2_runtime.playgate_generation,
                                   token, token_valid)) {
                g_switch2_runtime.playgate_status_subscribed =
                    event->subscribe.cur_notify != 0;
            }
            portEXIT_CRITICAL(&g_switch2_lock);
        }
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (role_event_matches(instance, event->passkey.conn_handle,
                               token, token_valid)) {
            (void)ble_gap_terminate(event->passkey.conn_handle,
                                    BLE_ERR_AUTH_FAIL);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        if (role_event_matches(instance, event->repeat_pairing.conn_handle,
                               token, token_valid)) {
            (void)ble_gap_terminate(event->repeat_pairing.conn_handle,
                                    BLE_ERR_AUTH_FAIL);
        }
        return BLE_GAP_REPEAT_PAIRING_IGNORE;

    default:
        return 0;
    }
}

static void report_once(int64_t now)
{
    uint16_t conn_handle;
    uint64_t token;
    uint8_t counter;
    uint8_t feature_mask;
    portENTER_CRITICAL(&g_switch2_lock);
    const bool ready = g_switch2_runtime.console_ready &&
                       g_switch2_runtime.console_input_subscribed;
    conn_handle = g_switch2_runtime.console_conn_handle;
    token = g_switch2_runtime.console_generation;
    counter = g_switch2_runtime.report_counter;
    if (ready) {
        counter = ++g_switch2_runtime.report_counter;
    }
    feature_mask = g_switch2_runtime.feature_mask;
    portEXIT_CRITICAL(&g_switch2_lock);
    if (!ready || conn_handle == SWITCH2_NO_CONN) {
        return;
    }

    controller_state_t state;
    input_core_snapshot(now, &state, NULL);
    switch2_protocol_encode_input(counter, feature_mask, &state, s_report);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(s_report, sizeof(s_report));
    if (om == NULL) {
        switch2_metric_mbuf_unavailable();
        return;
    }

    portENTER_CRITICAL(&g_switch2_lock);
    const bool still_current = g_switch2_runtime.console_ready &&
        g_switch2_runtime.console_input_subscribed &&
        g_switch2_runtime.console_conn_handle == conn_handle &&
        g_switch2_runtime.console_generation == token;
    portEXIT_CRITICAL(&g_switch2_lock);
    if (!still_current) {
        os_mbuf_free_chain(om);
        return;
    }

    const int rc = ble_gatts_notify_custom(conn_handle,
                                            SWITCH2_INPUT_VALUE_HANDLE, om);
    if (rc == 0) {
        input_core_mark_reported(state.generation, now);
        portENTER_CRITICAL(&g_switch2_lock);
        increment_saturating(&g_switch2_runtime.metrics.report_submits);
        portEXIT_CRITICAL(&g_switch2_lock);
    } else {
        switch2_metric_notify_error();
    }
}

static void report_task(void *arg)
{
    (void)arg;
    const int64_t period_us = INT64_C(1000000) / CONFIG_PLAYGATE_REPORT_HZ;
    int64_t deadline = esp_timer_get_time() + period_us;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (now < deadline) {
            const int64_t wait_us = deadline - now;
            if (wait_us >= 1000) {
                vTaskDelay(pdMS_TO_TICKS((wait_us + 999) / 1000));
            } else {
                taskYIELD();
            }
            continue;
        }
        if (now - deadline >= period_us) {
            portENTER_CRITICAL(&g_switch2_lock);
            increment_saturating(&g_switch2_runtime.metrics.missed_deadlines);
            portEXIT_CRITICAL(&g_switch2_lock);
            deadline = now;
        }
        report_once(now);
        deadline += period_us;
    }
}

static void notify_start_result(esp_err_t result)
{
    s_start_result = result;
    if (s_start_waiter != NULL) {
        xTaskNotifyGive(s_start_waiter);
    }
}

static void host_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 ||
        !switch2_gatt_handles_valid() ||
        (g_switch2_pairing.record.paired &&
         switch2_pairing_inject(&g_switch2_pairing) != 0) ||
        switch2_start_console_advertising(
            g_switch2_pairing.record.paired) != 0) {
        notify_start_result(ESP_FAIL);
        (void)nimble_port_stop();
        return;
    }
    notify_start_result(ESP_OK);
}

static void host_reset(int reason)
{
    (void)reason;
    gateway_status_set_hid_state(GATEWAY_HID_IDLE);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t switch2_controller_start(void)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = switch2_pairing_load_or_create(&g_switch2_pairing);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_iface_mac_addr_set(g_switch2_pairing.record.controller_address,
                                 ESP_MAC_BT);
    if (err != ESP_OK) {
        return err;
    }

    memset(&g_switch2_runtime, 0, sizeof(g_switch2_runtime));
    g_switch2_runtime.console_conn_handle = SWITCH2_NO_CONN;
    g_switch2_runtime.playgate_conn_handle = SWITCH2_NO_CONN;
    switch2_protocol_init(&g_switch2_protocol,
                          g_switch2_pairing.record.controller_address);
    gateway_status_set_hid_state(GATEWAY_HID_IDLE);
    gateway_status_set_ble_connected(false);

    err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }
    if (ble_att_set_preferred_mtu(512) != 0) {
        (void)nimble_port_deinit();
        return ESP_FAIL;
    }

    ble_hs_cfg.reset_cb = host_reset;
    ble_hs_cfg.sync_cb = host_sync;
    ble_hs_cfg.gatts_register_cb = switch2_gatt_register_callback;
    ble_hs_cfg.gatts_register_arg = NULL;
    ble_hs_cfg.store_read_cb = store_read;
    ble_hs_cfg.store_write_cb = store_write;
    ble_hs_cfg.store_delete_cb = store_delete;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_sc_only = 0;
    ble_hs_cfg.sm_our_key_dist = 0;
    ble_hs_cfg.sm_their_key_dist = 0;

    if (switch2_gatt_register() != 0) {
        (void)nimble_port_deinit();
        return ESP_FAIL;
    }

    s_start_waiter = xTaskGetCurrentTaskHandle();
    s_start_result = ESP_FAIL;
    nimble_port_freertos_init(host_task);
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SWITCH2_START_TIMEOUT_MS)) == 0 ||
        s_start_result != ESP_OK) {
        s_start_waiter = NULL;
        (void)nimble_port_stop();
        return ESP_FAIL;
    }
    s_start_waiter = NULL;

    s_report_task = xTaskCreateStatic(report_task, "switch2_report",
                                      SWITCH2_REPORT_STACK_WORDS, NULL,
                                      8, s_report_stack, &s_report_tcb);
    if (s_report_task == NULL) {
        switch2_stop_playgate_advertising();
        (void)ble_gap_ext_adv_stop(SWITCH2_CONSOLE_ADV_INSTANCE);
        (void)nimble_port_stop();
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

bool switch2_controller_is_connected(void)
{
    bool connected;
    portENTER_CRITICAL(&g_switch2_lock);
    connected = g_switch2_runtime.console_ready;
    portEXIT_CRITICAL(&g_switch2_lock);
    return connected;
}

void switch2_controller_publish_playgate_status(const uint8_t status[20])
{
    if (status == NULL) {
        return;
    }
    uint16_t conn_handle;
    uint64_t token;
    bool notify;
    portENTER_CRITICAL(&g_switch2_lock);
    memcpy(g_switch2_runtime.status, status, sizeof(g_switch2_runtime.status));
    conn_handle = g_switch2_runtime.playgate_conn_handle;
    token = g_switch2_runtime.playgate_generation;
    notify = g_switch2_runtime.console_ready &&
             g_switch2_runtime.playgate_status_subscribed &&
             conn_handle != SWITCH2_NO_CONN &&
             g_switch2_playgate_status_handle != 0;
    portEXIT_CRITICAL(&g_switch2_lock);
    if (!notify) {
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(status, 20);
    if (om == NULL) {
        switch2_metric_mbuf_unavailable();
        return;
    }
    portENTER_CRITICAL(&g_switch2_lock);
    const bool current = g_switch2_runtime.console_ready &&
        g_switch2_runtime.playgate_status_subscribed &&
        g_switch2_runtime.playgate_conn_handle == conn_handle &&
        g_switch2_runtime.playgate_generation == token;
    portEXIT_CRITICAL(&g_switch2_lock);
    if (!current) {
        os_mbuf_free_chain(om);
        return;
    }
    if (ble_gatts_notify_custom(conn_handle,
                                g_switch2_playgate_status_handle, om) != 0) {
        switch2_metric_notify_error();
    }
}

void switch2_controller_get_metrics(switch2_controller_metrics_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&g_switch2_lock);
    *out = g_switch2_runtime.metrics;
    portEXIT_CRITICAL(&g_switch2_lock);
}
