#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool started; /*!< Wi-Fi/UDP transport started */
    bool connected;
    bool ap_mode;
    char ssid[33];
    char ip[16];
} input_transports_net_info_t;

esp_err_t input_transports_start_wifi_udp(void);
esp_err_t input_transports_start_serial(void);
esp_err_t input_transports_start_status_publisher(void);
void input_transports_publish_status_now(void);
void input_transports_get_net_info(input_transports_net_info_t *out);

#ifdef __cplusplus
}
#endif
