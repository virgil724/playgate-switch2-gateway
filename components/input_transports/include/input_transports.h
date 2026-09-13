#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t input_transports_start_wifi_udp(void);
esp_err_t input_transports_start_serial(void);
esp_err_t input_transports_start_status_publisher(void);
void input_transports_publish_status_now(void);

#ifdef __cplusplus
}
#endif
