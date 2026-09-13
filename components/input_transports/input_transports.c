#include "input_transports.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway_status.h"
#include "transport_internal.h"
#include "switch2_controller.h"

#define STATUS_PERIOD_US INT64_C(1000000)
#define STATUS_TASK_STACK_DEPTH 3072
#define STATUS_TASK_PRIORITY 5

static StaticTask_t s_status_task_tcb;
static StackType_t s_status_task_stack[STATUS_TASK_STACK_DEPTH];
static TaskHandle_t s_status_task;

static void publish_status(void)
{
    uint8_t status[GATEWAY_STATUS_SIZE];
    gateway_status_encode(esp_timer_get_time(), status);
    serial_input_publish_status(status, sizeof(status));
    switch2_controller_publish_playgate_status(status);
}

static void status_task(void *context)
{
    (void)context;
    int64_t next_periodic_us = esp_timer_get_time();

    for (;;) {
        const int64_t now_us = esp_timer_get_time();
        if (now_us >= next_periodic_us) {
            publish_status();
            next_periodic_us = now_us + STATUS_PERIOD_US;
        }

        const int64_t remaining_us = next_periodic_us - esp_timer_get_time();
        const uint32_t wait_ms = remaining_us > 0
                                     ? (uint32_t)((remaining_us + 999) / 1000)
                                     : 0u;
        TickType_t wait_ticks = pdMS_TO_TICKS(wait_ms);
        if (wait_ticks == 0) {
            wait_ticks = 1;
        }
        if (ulTaskNotifyTake(pdTRUE, wait_ticks) != 0u) {
            publish_status();
        }
    }
}

void input_transports_publish_status_now(void)
{
    TaskHandle_t task = s_status_task;
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
}

static void status_changed(void *context)
{
    (void)context;
    input_transports_publish_status_now();
}

esp_err_t input_transports_start_status_publisher(void)
{
    if (s_status_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    TaskHandle_t task = xTaskCreateStatic(status_task,
                                          "gateway_status",
                                          STATUS_TASK_STACK_DEPTH,
                                          NULL,
                                          STATUS_TASK_PRIORITY,
                                          s_status_task_stack,
                                          &s_status_task_tcb);
    if (task == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_status_task = task;
    gateway_status_set_changed_callback(status_changed, NULL);
    xTaskNotifyGive(task);
    return ESP_OK;
}
