#include "input_transports.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controller_core.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway_status.h"
#include "sdkconfig.h"
#include "serial_parser.h"
#include "transport_internal.h"

#if CONFIG_PLAYGATE_SERIAL_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#elif CONFIG_PLAYGATE_SERIAL_UART
#include "driver/uart.h"
#endif

#define PLAYGATE_UART_PORT UART_NUM_0
#define SERIAL_RX_RING_SIZE 1024
#define SERIAL_TX_RING_SIZE 128
#define SERIAL_TX_TIMEOUT_MS 5u
#define SERIAL_IO_CHUNK_SIZE 128
#define SERIAL_TASK_STACK_DEPTH 3072
#define SERIAL_TASK_PRIORITY 9

static StaticTask_t s_serial_task_tcb;
static StackType_t s_serial_task_stack[SERIAL_TASK_STACK_DEPTH];
static volatile bool s_serial_started;

static void serial_frame_received(uint8_t type,
                                  const uint8_t *payload,
                                  size_t payload_len,
                                  void *context)
{
    (void)context;
    if (type == SERIAL_FRAME_TYPE_INPUT && payload_len == PLAYGATE_INPUT_PAYLOAD_SIZE) {
        (void)input_core_submit(INPUT_SOURCE_SERIAL,
                                payload,
                                payload_len,
                                esp_timer_get_time());
    }
}

static int serial_backend_read(uint8_t *data, size_t capacity)
{
#if CONFIG_PLAYGATE_SERIAL_USB_SERIAL_JTAG
    return usb_serial_jtag_read_bytes(data, capacity, portMAX_DELAY);
#elif CONFIG_PLAYGATE_SERIAL_UART
    return uart_read_bytes(PLAYGATE_UART_PORT,
                           data,
                           capacity,
                           portMAX_DELAY);
#else
    (void)data;
    (void)capacity;
    return 0;
#endif
}

static void serial_backend_write(const uint8_t *data, size_t len)
{
#if CONFIG_PLAYGATE_SERIAL_USB_SERIAL_JTAG
    if (!usb_serial_jtag_is_connected() || len == 0u || len > SERIAL_TX_RING_SIZE) {
        return;
    }
    (void)usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(SERIAL_TX_TIMEOUT_MS));
#elif CONFIG_PLAYGATE_SERIAL_UART
    (void)uart_write_bytes(PLAYGATE_UART_PORT, data, len);
#else
    (void)data;
    (void)len;
#endif
}

static void serial_input_task(void *context)
{
    (void)context;
    serial_parser_t parser;
    uint8_t chunk[SERIAL_IO_CHUNK_SIZE];
    serial_parser_init(&parser, serial_frame_received, NULL);

    for (;;) {
        const int received = serial_backend_read(chunk, sizeof(chunk));
        if (received > 0) {
            serial_parser_feed(&parser, chunk, (size_t)received);
        }
    }
}

static esp_err_t serial_backend_start(void)
{
#if CONFIG_PLAYGATE_SERIAL_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = SERIAL_TX_RING_SIZE,
        .rx_buffer_size = SERIAL_RX_RING_SIZE,
    };
    return usb_serial_jtag_driver_install(&config);
#elif CONFIG_PLAYGATE_SERIAL_UART
    const uart_config_t config = {
        .baud_rate = CONFIG_PLAYGATE_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .rx_glitch_filt_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {
            .allow_pd = 0,
            .backup_before_sleep = 0,
        },
    };

    esp_err_t err = uart_param_config(PLAYGATE_UART_PORT, &config);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_pin(PLAYGATE_UART_PORT,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }
    return uart_driver_install(PLAYGATE_UART_PORT,
                               SERIAL_RX_RING_SIZE,
                               0,
                               0,
                               NULL,
                               0);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void serial_backend_stop(void)
{
#if CONFIG_PLAYGATE_SERIAL_USB_SERIAL_JTAG
    (void)usb_serial_jtag_driver_uninstall();
#elif CONFIG_PLAYGATE_SERIAL_UART
    (void)uart_driver_delete(PLAYGATE_UART_PORT);
#endif
}

esp_err_t input_transports_start_serial(void)
{
#if !CONFIG_PLAYGATE_ENABLE_SERIAL
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_serial_started) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = serial_backend_start();
    if (err != ESP_OK) {
        return err;
    }

    TaskHandle_t task = xTaskCreateStatic(serial_input_task,
                                          "playgate_serial",
                                          SERIAL_TASK_STACK_DEPTH,
                                          NULL,
                                          SERIAL_TASK_PRIORITY,
                                          s_serial_task_stack,
                                          &s_serial_task_tcb);
    if (task == NULL) {
        serial_backend_stop();
        return ESP_ERR_NO_MEM;
    }

    s_serial_started = true;
    return ESP_OK;
#endif
}

void serial_input_publish_status(const uint8_t *status, size_t len)
{
#if CONFIG_PLAYGATE_ENABLE_SERIAL
    if (!s_serial_started || status == NULL || len != GATEWAY_STATUS_SIZE) {
        return;
    }

    uint8_t frame[SERIAL_FRAME_MAX_ENCODED_SIZE];
    const size_t frame_len = serial_frame_encode(SERIAL_FRAME_TYPE_STATUS,
                                                  status,
                                                  len,
                                                  frame,
                                                  sizeof(frame));
    if (frame_len != 0u) {
        serial_backend_write(frame, frame_len);
    }
#else
    (void)status;
    (void)len;
#endif
}
