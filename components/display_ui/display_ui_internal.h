#pragma once

#include <stdint.h>

#include "controller_core.h"
#include "esp_err.h"
#include "gateway_status.h"
#include "input_transports.h"
#include "switch2_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    input_metrics_t metrics;
    gateway_status_view_t link;
    switch2_controller_metrics_t console;
    input_transports_net_info_t net;
    uint32_t free_heap;
    int64_t uptime_us;
} display_ui_snapshot_t;

void display_ui_snapshot_take(display_ui_snapshot_t *out);

esp_err_t display_ui_port_init(void);
esp_err_t display_ui_port_lvgl_init(void);

void display_ui_view_create(void);
void display_ui_view_update(const display_ui_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
