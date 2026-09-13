/*
 * Nintendo GATT UUIDs, ordering, permissions, and fixed values are ported from
 * https://github.com/zhantss/ESP32-BLE5-NSController-Emulator at pinned commit
 * 0ea0c62aeab2440c7f9c77f29cf87ffa3b4c192f (MIT; bundled notice).
 */
#include "switch2_internal.h"

#include <string.h>

#include "esp_timer.h"
#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "os/os_mbuf.h"

#define UUID128(name, ...) static const ble_uuid128_t name = BLE_UUID128_INIT(__VA_ARGS__)

UUID128(s_svc_0001, 0x80,0xd2,0x6b,0xf9,0x56,0x19,0x51,0x8f,0x30,0x4e,0x64,0x19,0x5d,0xaf,0xc5,0x00);
UUID128(s_chr_0003, 0x81,0xd2,0x6b,0xf9,0x56,0x19,0x51,0x8f,0x30,0x4e,0x64,0x19,0x5d,0xaf,0xc5,0x00);
UUID128(s_chr_0005, 0x82,0xd2,0x6b,0xf9,0x56,0x19,0x51,0x8f,0x30,0x4e,0x64,0x19,0x5d,0xaf,0xc5,0x00);
UUID128(s_chr_0007, 0x83,0xd2,0x6b,0xf9,0x56,0x19,0x51,0x8f,0x30,0x4e,0x64,0x19,0x5d,0xaf,0xc5,0x00);
UUID128(s_svc_0008, 0xd0,0x7f,0xdf,0x09,0x8f,0x11,0x8f,0x82,0xad,0x49,0xfe,0x89,0xbe,0xe9,0x7d,0xab);
UUID128(s_chr_000a, 0xd2,0x7f,0xdf,0x09,0x8f,0x11,0x8f,0x82,0xad,0x49,0xfe,0x89,0xbe,0xe9,0x7d,0xab);
UUID128(s_chr_000e, 0xf8,0xc0,0xfc,0x5f,0x75,0x32,0x58,0x82,0x19,0x46,0x3e,0xec,0x6c,0x86,0x92,0x74);
UUID128(s_chr_0012, 0x05,0x2b,0xf7,0x31,0x0c,0x63,0x39,0xa9,0x7d,0x42,0x58,0x92,0x51,0x3f,0x48,0xcc);
UUID128(s_chr_0014, 0x05,0xf0,0xe5,0x4f,0xa5,0x1e,0x44,0xaf,0x6c,0x4e,0xb7,0x8e,0xc9,0x4a,0x9d,0x64);
UUID128(s_chr_0016, 0x79,0xb3,0xe8,0x09,0x98,0x6f,0xaf,0x8e,0xb5,0x40,0x55,0x69,0x7e,0xbc,0xac,0x3d);
UUID128(s_chr_0018, 0x8d,0x9f,0xf5,0x5d,0x3e,0xd2,0xf7,0xa4,0xf7,0x4d,0xae,0xfd,0x3d,0x42,0x47,0x41);
UUID128(s_chr_001a, 0x6a,0x83,0x11,0xb1,0x15,0x53,0x0a,0xa2,0x36,0x4d,0xd8,0xd9,0x61,0xa9,0x65,0xc7);
UUID128(s_chr_001e, 0xe0,0x57,0x76,0xa7,0x6b,0x32,0x49,0xa5,0x95,0x4e,0x78,0x42,0x7d,0x9f,0x6d,0x50);
UUID128(s_chr_0022, 0x80,0x2a,0x6d,0x40,0x6f,0xf8,0x15,0xab,0x41,0x42,0x1c,0x84,0xd2,0x69,0xbd,0xd3);
UUID128(s_chr_0026, 0xde,0x7f,0xdf,0x09,0x8f,0x11,0x8f,0x82,0xad,0x49,0xfe,0x89,0xbe,0xe9,0x7d,0xab);
UUID128(s_chr_002a, 0xdf,0x7f,0xdf,0x09,0x8f,0x11,0x8f,0x82,0xad,0x49,0xfe,0x89,0xbe,0xe9,0x7d,0xab);
UUID128(s_chr_002c, 0x06,0x2b,0xf7,0x31,0x0c,0x63,0x39,0xa9,0x7d,0x42,0x58,0x92,0x51,0x3f,0x48,0xcc);
UUID128(s_chr_002e, 0xf9,0xc0,0xfc,0x5f,0x75,0x32,0x58,0x82,0x19,0x46,0x3e,0xec,0x6c,0x86,0x92,0x74);
UUID128(s_chr_0032, 0x80,0xb3,0xe8,0x09,0x98,0x6f,0xaf,0x8e,0xb5,0x40,0x55,0x69,0x7e,0xbc,0xac,0x3d);
UUID128(s_dsc_rate, 0xcb,0x6e,0x48,0x80,0xdf,0x95,0x57,0x95,0xee,0x4d,0x24,0x5a,0x10,0x55,0x9d,0x67);
UUID128(s_dsc_unknown, 0x79,0xf9,0xa4,0xed,0xbb,0xe3,0xd2,0x9c,0x5b,0x49,0x58,0xf3,0x8c,0xdf,0x46,0xb7);
UUID128(s_playgate_svc, 0xe1,0xf1,0xd7,0xa9,0x70,0x4c,0x7a,0x9b,0x93,0x4f,0x4d,0x3b,0x2e,0x9c,0x5c,0x5f);
UUID128(s_playgate_input, 0xe2,0xf1,0xd7,0xa9,0x70,0x4c,0x7a,0x9b,0x93,0x4f,0x4d,0x3b,0x2e,0x9c,0x5c,0x5f);
UUID128(s_playgate_status, 0xe3,0xf1,0xd7,0xa9,0x70,0x4c,0x7a,0x9b,0x93,0x4f,0x4d,0x3b,0x2e,0x9c,0x5c,0x5f);

static uint16_t s_nintendo_handles[SWITCH2_CONSOLE_VALUE_HANDLE_COUNT];
uint16_t g_switch2_playgate_input_handle;
uint16_t g_switch2_playgate_status_handle;
static bool s_handles_valid;
static size_t s_value_index;
static size_t s_descriptor_index;
static size_t s_service_index;

static const uint16_t s_descriptor_handles[] = {
    0x000c, 0x0010, 0x001c, 0x0020, 0x0024, 0x0028, 0x0030,
};

static bool current_console(uint16_t conn_handle, uint64_t *generation)
{
    bool valid;
    portENTER_CRITICAL(&g_switch2_lock);
    valid = conn_handle == g_switch2_runtime.console_conn_handle &&
            conn_handle != SWITCH2_NO_CONN;
    if (generation != NULL) {
        *generation = g_switch2_runtime.console_generation;
    }
    portEXIT_CRITICAL(&g_switch2_lock);
    return valid;
}

static bool current_playgate(uint16_t conn_handle)
{
    bool valid;
    portENTER_CRITICAL(&g_switch2_lock);
    valid = conn_handle == g_switch2_runtime.playgate_conn_handle &&
            conn_handle != SWITCH2_NO_CONN && g_switch2_runtime.console_ready;
    portEXIT_CRITICAL(&g_switch2_lock);
    return valid;
}

static int append(struct os_mbuf *om, const void *bytes, size_t len)
{
    return os_mbuf_append(om, bytes, len) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int descriptor_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->offset != 0) {
        return BLE_ATT_ERR_INVALID_OFFSET;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC ||
        ctxt->op == BLE_GATT_ACCESS_OP_WRITE_DSC) {
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int fixed_access(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)arg;
    static const uint8_t value_0003[] = {0x04,0x00,0x05,0x00,0x01,0x01,0x00};
    static const uint8_t value_0007[] = {0x36,0x80,0x74,0xee,0xbb,0x3d,0x8e,0x13};
    if (!current_console(conn_handle, NULL) || ctxt->offset != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (attr_handle == s_nintendo_handles[0]) {
            return append(ctxt->om, value_0003, sizeof(value_0003));
        }
        if (attr_handle == s_nintendo_handles[2]) {
            return append(ctxt->om, value_0007, sizeof(value_0007));
        }
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR &&
        attr_handle == s_nintendo_handles[1] && OS_MBUF_PKTLEN(ctxt->om) == 2) {
        return 0;
    }
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
}

static int generic_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)arg;
    if (!current_console(conn_handle, NULL) || ctxt->offset != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (attr_handle == SWITCH2_INPUT_VALUE_HANDLE) {
            controller_state_t state;
            uint8_t report[SWITCH2_INPUT_REPORT_SIZE];
            uint8_t counter;
            uint8_t features;
            input_core_snapshot(esp_timer_get_time(), &state, NULL);
            portENTER_CRITICAL(&g_switch2_lock);
            counter = g_switch2_runtime.report_counter;
            features = g_switch2_runtime.feature_mask;
            portEXIT_CRITICAL(&g_switch2_lock);
            switch2_protocol_encode_input(counter, features, &state, report);
            return append(ctxt->om, report, sizeof(report));
        }
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int notify_response(uint16_t conn_handle, uint16_t value_handle,
                           const uint8_t *bytes, size_t len)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(bytes, len);
    if (om == NULL) {
        switch2_metric_mbuf_unavailable();
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    const int rc = ble_gatts_notify_custom(conn_handle, value_handle, om);
    if (rc != 0) {
        switch2_metric_notify_error();
        return BLE_ATT_ERR_UNLIKELY;
    }
    return 0;
}

static int command_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)arg;
    uint64_t generation;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || ctxt->offset != 0 ||
        !current_console(conn_handle, &generation)) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    const size_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > SWITCH2_COMMAND_RESPONSE_MAX_SIZE) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint8_t request[SWITCH2_COMMAND_RESPONSE_MAX_SIZE];
    if (os_mbuf_copydata(ctxt->om, 0, len, request) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    switch2_command_endpoint_t endpoint;
    uint16_t response_handle;
    if (attr_handle == SWITCH2_BASIC_COMMAND_VALUE_HANDLE) {
        endpoint = SWITCH2_COMMAND_BASIC;
        response_handle = SWITCH2_BASIC_RESPONSE_VALUE_HANDLE;
    } else if (attr_handle == SWITCH2_COMBINED_COMMAND_VALUE_HANDLE) {
        endpoint = SWITCH2_COMMAND_COMBINED;
        response_handle = SWITCH2_COMBINED_RESPONSE_VALUE_HANDLE;
    } else {
        return 0;
    }

    uint8_t response[SWITCH2_COMMAND_RESPONSE_MAX_SIZE];
    size_t response_len;
    switch2_command_effects_t effects;
    const switch2_command_result_t result = switch2_protocol_process_command(
        &g_switch2_protocol, generation, endpoint, request, len,
        response, sizeof(response), &response_len, &effects);
    if (result != SWITCH2_COMMAND_OK) {
        return result == SWITCH2_COMMAND_RESPONSE_TOO_SMALL
                   ? BLE_ATT_ERR_INSUFFICIENT_RES
                   : BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    if (effects.pairing_finalized || effects.direct_pairing_received) {
        if (switch2_pairing_commit(&g_switch2_pairing, effects.console_address,
                                   effects.ltk) != ESP_OK ||
            switch2_pairing_inject(&g_switch2_pairing) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
    }
    if (effects.clear_pairing) {
        if (switch2_pairing_clear(&g_switch2_pairing) != ESP_OK) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        switch2_runtime_pairing_cleared();
    }
    if (effects.feature_activation) {
        switch2_runtime_feature_activated(effects.feature_mask);
    }

    if (!current_console(conn_handle, NULL)) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return notify_response(conn_handle, response_handle, response, response_len);
}

static int playgate_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)arg;
    if (!current_playgate(conn_handle) || ctxt->offset != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (attr_handle == g_switch2_playgate_input_handle &&
        ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (OS_MBUF_PKTLEN(ctxt->om) != PLAYGATE_INPUT_PAYLOAD_SIZE) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        uint8_t payload[PLAYGATE_INPUT_PAYLOAD_SIZE];
        if (os_mbuf_copydata(ctxt->om, 0, sizeof(payload), payload) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        return input_core_submit(INPUT_SOURCE_BLE, payload, sizeof(payload),
                                 esp_timer_get_time()) == INPUT_SUBMIT_INVALID
                   ? BLE_ATT_ERR_VALUE_NOT_ALLOWED
                   : 0;
    }
    if (attr_handle == g_switch2_playgate_status_handle &&
        ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t status[20];
        portENTER_CRITICAL(&g_switch2_lock);
        memcpy(status, g_switch2_runtime.status, sizeof(status));
        portEXIT_CRITICAL(&g_switch2_lock);
        return append(ctxt->om, status, sizeof(status));
    }
    return BLE_ATT_ERR_UNLIKELY;
}

#define RATE_DESCRIPTORS_RW ((struct ble_gatt_dsc_def[]){{.uuid=&s_dsc_rate.u,.att_flags=BLE_ATT_F_READ|BLE_ATT_F_WRITE,.access_cb=descriptor_access},{0}})
#define UNKNOWN_DESCRIPTORS ((struct ble_gatt_dsc_def[]){{.uuid=&s_dsc_unknown.u,.att_flags=BLE_ATT_F_READ|BLE_ATT_F_WRITE,.access_cb=descriptor_access},{0}})

static const struct ble_gatt_svc_def s_services[] = {
    {.type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=&s_svc_0001.u,.characteristics=(struct ble_gatt_chr_def[]){
        {.uuid=&s_chr_0003.u,.flags=BLE_GATT_CHR_F_READ,.val_handle=&s_nintendo_handles[0],.access_cb=fixed_access},
        {.uuid=&s_chr_0005.u,.flags=BLE_GATT_CHR_F_WRITE,.val_handle=&s_nintendo_handles[1],.access_cb=fixed_access},
        {.uuid=&s_chr_0007.u,.flags=BLE_GATT_CHR_F_READ,.val_handle=&s_nintendo_handles[2],.access_cb=fixed_access},{0}}},
    {.type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=&s_svc_0008.u,.characteristics=(struct ble_gatt_chr_def[]){
        {.uuid=&s_chr_000a.u,.flags=BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_NOTIFY,.val_handle=&s_nintendo_handles[3],.access_cb=generic_access,.descriptors=RATE_DESCRIPTORS_RW},
        {.uuid=&s_chr_000e.u,.flags=BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_NOTIFY,.val_handle=&s_nintendo_handles[4],.access_cb=generic_access,.descriptors=RATE_DESCRIPTORS_RW},
        {.uuid=&s_chr_0012.u,.flags=BLE_GATT_CHR_F_WRITE_NO_RSP,.val_handle=&s_nintendo_handles[5],.access_cb=generic_access},
        {.uuid=&s_chr_0014.u,.flags=BLE_GATT_CHR_F_WRITE_NO_RSP,.val_handle=&s_nintendo_handles[6],.access_cb=command_access},
        {.uuid=&s_chr_0016.u,.flags=BLE_GATT_CHR_F_WRITE_NO_RSP,.val_handle=&s_nintendo_handles[7],.access_cb=command_access},
        {.uuid=&s_chr_0018.u,.flags=BLE_GATT_CHR_F_WRITE_NO_RSP,.val_handle=&s_nintendo_handles[8],.access_cb=generic_access},
        {.uuid=&s_chr_001a.u,.flags=BLE_GATT_CHR_F_NOTIFY,.val_handle=&s_nintendo_handles[9],.access_cb=generic_access,.descriptors=UNKNOWN_DESCRIPTORS},
        {.uuid=&s_chr_001e.u,.flags=BLE_GATT_CHR_F_NOTIFY,.val_handle=&s_nintendo_handles[10],.access_cb=generic_access,.descriptors=UNKNOWN_DESCRIPTORS},
        {.uuid=&s_chr_0022.u,.flags=BLE_GATT_CHR_F_NOTIFY,.val_handle=&s_nintendo_handles[11],.access_cb=generic_access,.descriptors=UNKNOWN_DESCRIPTORS},
        {.uuid=&s_chr_0026.u,.flags=BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_NOTIFY,.val_handle=&s_nintendo_handles[12],.access_cb=generic_access,.descriptors=RATE_DESCRIPTORS_RW},
        {.uuid=&s_chr_002a.u,.flags=BLE_GATT_CHR_F_WRITE_NO_RSP,.val_handle=&s_nintendo_handles[13],.access_cb=generic_access},
        {.uuid=&s_chr_002c.u,.flags=BLE_GATT_CHR_F_WRITE_NO_RSP,.val_handle=&s_nintendo_handles[14],.access_cb=generic_access},
        {.uuid=&s_chr_002e.u,.flags=BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_NOTIFY,.val_handle=&s_nintendo_handles[15],.access_cb=generic_access,.descriptors=(struct ble_gatt_dsc_def[]){{.uuid=&s_dsc_rate.u,.att_flags=BLE_ATT_F_READ,.access_cb=descriptor_access},{0}}},
        {.uuid=&s_chr_0032.u,.flags=BLE_GATT_CHR_F_WRITE_NO_RSP,.val_handle=&s_nintendo_handles[16],.access_cb=generic_access},{0}}},
    {.type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=&s_playgate_svc.u,.characteristics=(struct ble_gatt_chr_def[]){
        {.uuid=&s_playgate_input.u,.flags=BLE_GATT_CHR_F_WRITE|BLE_GATT_CHR_F_WRITE_NO_RSP,.val_handle=&g_switch2_playgate_input_handle,.access_cb=playgate_access},
        {.uuid=&s_playgate_status.u,.flags=BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_NOTIFY,.val_handle=&g_switch2_playgate_status_handle,.access_cb=playgate_access},{0}}},
    {0},
};

void switch2_gatt_register_callback(struct ble_gatt_register_ctxt *ctxt,
                                    void *arg)
{
    (void)arg;
    if (ctxt == NULL) {
        s_handles_valid = false;
        return;
    }
    if (ctxt->op == BLE_GATT_REGISTER_OP_SVC) {
        static const uint16_t expected[] = {0x0001, 0x0008, 0x0033};
        if (s_service_index >= sizeof(expected) / sizeof(expected[0]) ||
            ctxt->svc.handle != expected[s_service_index++]) {
            s_handles_valid = false;
        }
    } else if (ctxt->op == BLE_GATT_REGISTER_OP_CHR) {
        uint16_t expected;
        if (s_value_index < SWITCH2_CONSOLE_VALUE_HANDLE_COUNT) {
            expected = switch2_console_value_handles[s_value_index];
        } else if (s_value_index == SWITCH2_CONSOLE_VALUE_HANDLE_COUNT) {
            expected = 0x0035;
        } else if (s_value_index == SWITCH2_CONSOLE_VALUE_HANDLE_COUNT + 1) {
            expected = 0x0037;
        } else {
            s_handles_valid = false;
            return;
        }
        if (ctxt->chr.val_handle != expected) {
            s_handles_valid = false;
        }
        ++s_value_index;
    } else if (ctxt->op == BLE_GATT_REGISTER_OP_DSC) {
        if (s_descriptor_index >= sizeof(s_descriptor_handles) / sizeof(s_descriptor_handles[0]) ||
            ctxt->dsc.handle != s_descriptor_handles[s_descriptor_index++]) {
            s_handles_valid = false;
        }
    }
}

int switch2_gatt_register(void)
{
    memset(s_nintendo_handles, 0, sizeof(s_nintendo_handles));
    g_switch2_playgate_input_handle = 0;
    g_switch2_playgate_status_handle = 0;
    s_handles_valid = true;
    s_value_index = 0;
    s_descriptor_index = 0;
    s_service_index = 0;
    int rc = ble_gatts_count_cfg(s_services);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_services);
    }
    return rc;
}

bool switch2_gatt_handles_valid(void)
{
    if (!s_handles_valid || s_service_index != 3 ||
        s_value_index != SWITCH2_CONSOLE_VALUE_HANDLE_COUNT + 2 ||
        s_descriptor_index != sizeof(s_descriptor_handles) / sizeof(s_descriptor_handles[0]) ||
        g_switch2_playgate_input_handle != 0x0035 ||
        g_switch2_playgate_status_handle != 0x0037) {
        return false;
    }
    for (size_t i = 0; i < SWITCH2_CONSOLE_VALUE_HANDLE_COUNT; ++i) {
        if (s_nintendo_handles[i] != switch2_console_value_handles[i]) {
            return false;
        }
    }
    return true;
}

