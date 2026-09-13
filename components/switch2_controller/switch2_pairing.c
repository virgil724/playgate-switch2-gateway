/*
 * Nintendo pseudo-OOB pairing behavior is based on public protocol observations
 * and the MIT implementation pinned in the bundled provenance notice.
 */
#include "switch2_internal.h"

#include <string.h>

#include "esp_random.h"
#include "host/ble_store.h"
#include "nvs.h"

#define SWITCH2_NVS_NAMESPACE "switch2"
#define SWITCH2_NVS_PAIRING_KEY "pairing"

static bool nonzero(const uint8_t *bytes, size_t len)
{
    uint8_t value = 0;
    for (size_t i = 0; i < len; ++i) {
        value |= bytes[i];
    }
    return value != 0;
}

static void prepare_security(switch2_pairing_store_t *store)
{
    memset(&store->security, 0, sizeof(store->security));
    if (!store->record.paired) {
        return;
    }
    store->security.peer_addr.type = BLE_ADDR_PUBLIC;
    memcpy(store->security.peer_addr.val, store->record.console_address, 6);
#if MYNEWT_VAL(BLE_STORE_MAX_BONDS)
    store->security.bond_count = 1;
#endif
    store->security.key_size = 16;
    store->security.ediv = 0;
    store->security.rand_num = 0;
    /* The store key is the derived LTK verbatim: upstream injects ltk_re,
     * which equals A1 ^ B1, the exact value this record holds. */
    memcpy(store->security.ltk, store->record.ltk,
           sizeof(store->security.ltk));
    store->security.ltk_present = 1;
    store->security.authenticated = 1;
    store->security.sc = 1;
}

static esp_err_t save_record(nvs_handle_t handle,
                             const switch2_pairing_record_t *record)
{
    uint8_t bytes[SWITCH2_PAIRING_RECORD_SIZE];
    switch2_protocol_pairing_record_encode(record, bytes);
    esp_err_t err = nvs_set_blob(handle, SWITCH2_NVS_PAIRING_KEY,
                                 bytes, sizeof(bytes));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    return err;
}

esp_err_t switch2_pairing_load_or_create(switch2_pairing_store_t *store)
{
    if (store == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(store, 0, sizeof(*store));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SWITCH2_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t bytes[SWITCH2_PAIRING_RECORD_SIZE] = {0};
    size_t len = sizeof(bytes);
    err = nvs_get_blob(handle, SWITCH2_NVS_PAIRING_KEY, bytes, &len);
    if (err == ESP_OK &&
        switch2_protocol_pairing_record_decode(bytes, len, &store->record)) {
        prepare_security(store);
        nvs_close(handle);
        return ESP_OK;
    }

    switch2_pairing_record_t unpaired = {0};
    if (len == sizeof(bytes) && bytes[5] == 0x78 &&
        bytes[6] == 0x81 && bytes[7] == 0x8c) {
        memcpy(unpaired.controller_address, &bytes[5], 6);
    } else {
        unpaired.controller_address[0] = 0x78;
        unpaired.controller_address[1] = 0x81;
        unpaired.controller_address[2] = 0x8c;
        esp_fill_random(&unpaired.controller_address[3], 3);
        if (!nonzero(&unpaired.controller_address[3], 3)) {
            unpaired.controller_address[5] = 1;
        }
    }

    err = save_record(handle, &unpaired);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }
    store->record = unpaired;
    prepare_security(store);
    return ESP_OK;
}

esp_err_t switch2_pairing_commit(switch2_pairing_store_t *store,
                                 const uint8_t console_address[6],
                                 const uint8_t ltk[16])
{
    if (store == NULL || console_address == NULL || ltk == NULL ||
        !nonzero(console_address, 6) || !nonzero(ltk, 16)) {
        return ESP_ERR_INVALID_ARG;
    }

    switch2_pairing_record_t candidate = {0};
    memcpy(candidate.controller_address, store->record.controller_address, 6);
    memcpy(candidate.console_address, console_address, 6);
    memcpy(candidate.ltk, ltk, 16);
    candidate.paired = true;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SWITCH2_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = save_record(handle, &candidate);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }

    store->record = candidate;
    prepare_security(store);
    return ESP_OK;
}

esp_err_t switch2_pairing_clear(switch2_pairing_store_t *store)
{
    if (store == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    switch2_pairing_record_t candidate = {0};
    memcpy(candidate.controller_address, store->record.controller_address, 6);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SWITCH2_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = save_record(handle, &candidate);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }

    switch2_pairing_delete_injected(store);
    store->record = candidate;
    prepare_security(store);
    return ESP_OK;
}

int switch2_pairing_inject(const switch2_pairing_store_t *store)
{
    if (store == NULL || !store->record.paired) {
        return 0;
    }
    int rc = ble_store_write_our_sec(&store->security);
    if (rc != 0) {
        return rc;
    }
    rc = ble_store_write_peer_sec(&store->security);
    if (rc != 0) {
        struct ble_store_key_sec key = {0};
        key.peer_addr = store->security.peer_addr;
        (void)ble_store_delete_our_sec(&key);
    }
    return rc;
}

void switch2_pairing_delete_injected(const switch2_pairing_store_t *store)
{
    if (store == NULL || !store->record.paired) {
        return;
    }
    struct ble_store_key_sec key = {0};
    key.peer_addr.type = BLE_ADDR_PUBLIC;
    memcpy(key.peer_addr.val, store->record.console_address, 6);
    (void)ble_store_delete_our_sec(&key);
    (void)ble_store_delete_peer_sec(&key);
}
