#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t console_reconnects;
    uint32_t report_submits;
    uint32_t notify_errors;
    uint32_t mbuf_unavailable;
    uint32_t missed_deadlines;
} switch2_controller_metrics_t;

esp_err_t switch2_controller_start(void);
bool switch2_controller_is_connected(void);
void switch2_controller_publish_playgate_status(const uint8_t status[20]);
void switch2_controller_get_metrics(switch2_controller_metrics_t *out);

#ifdef __cplusplus
}
#endif
