#include "display_ui.h"

#include <stddef.h>

#include "display_ui_internal.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"

#if !CONFIG_PLAYGATE_ENABLE_DISPLAY

esp_err_t display_ui_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#else

#define DISPLAY_TASK_STACK_WORDS 4096 /* 16 KB, matching the other static tasks here */
#define DISPLAY_TASK_PRIORITY 4       /* below gateway_status, udp, serial and switch2_report */

static StaticTask_t s_task_tcb;
static StackType_t s_task_stack[DISPLAY_TASK_STACK_WORDS];
static TaskHandle_t s_task;

static int64_t s_next_values_us;

void display_ui_snapshot_take(display_ui_snapshot_t *out)
{
    input_core_snapshot(esp_timer_get_time(), NULL, &out->metrics);
    gateway_status_get_view(&out->link);
    switch2_controller_get_metrics(&out->console);
    input_transports_get_net_info(&out->net);
    out->free_heap = esp_get_free_heap_size();
    out->uptime_us = esp_timer_get_time();
}

static void display_task(void *context)
{
    (void)context;

    display_ui_view_create();

    int64_t last_us = esp_timer_get_time();
    s_next_values_us = last_us;
    display_ui_snapshot_t snapshot;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_PLAYGATE_DISPLAY_REFRESH_MS));

        const int64_t now = esp_timer_get_time();
        lv_tick_inc((uint32_t)((now - last_us) / 1000));
        last_us = now;

        if (now >= s_next_values_us) {
            display_ui_snapshot_take(&snapshot);
            display_ui_view_update(&snapshot);
            s_next_values_us = now + (int64_t)CONFIG_PLAYGATE_DISPLAY_VALUE_MS * 1000;
        }

        lv_timer_handler();
    }
}

esp_err_t display_ui_start(void)
{
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = display_ui_port_init();
    if (error != ESP_OK) {
        return error;
    }
    error = display_ui_port_lvgl_init();
    if (error != ESP_OK) {
        return error;
    }

    TaskHandle_t task = xTaskCreateStatic(display_task,
                                          "playgate_ui",
                                          DISPLAY_TASK_STACK_WORDS,
                                          NULL,
                                          DISPLAY_TASK_PRIORITY,
                                          s_task_stack,
                                          &s_task_tcb);
    if (task == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_task = task;
    return ESP_OK;
}

#endif /* CONFIG_PLAYGATE_ENABLE_DISPLAY */
