#include <limits.h>
#include <stdint.h>

#include "controller_core.h"
#include "esp_err.h"
#include "esp_log.h"
#include "gateway_status.h"
#include "input_transports.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "switch2_controller.h"
#include "web_console.h"

static const char *TAG = "playgate_gateway";
static uint32_t s_transport_start_failures;

static void record_transport_failure(const char *name, esp_err_t error)
{
    if (s_transport_start_failures != UINT32_MAX) {
        ++s_transport_start_failures;
    }
    ESP_LOGE(TAG, "%s initialization failed: %s", name, esp_err_to_name(error));
}

static esp_err_t initialize_nvs(void)
{
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        error = nvs_flash_erase();
        if (error == ESP_OK) {
            error = nvs_flash_init();
        }
    }
    return error;
}

void app_main(void)
{
    esp_err_t error = initialize_nvs();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialization failed: %s", esp_err_to_name(error));
        return;
    }

    input_core_init();
    gateway_status_init();

    error = switch2_controller_start();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Switch 2 controller initialization failed: %s",
                 esp_err_to_name(error));
        return;
    }

#if CONFIG_PLAYGATE_ENABLE_SERIAL
    error = input_transports_start_serial();
    if (error != ESP_OK) {
        record_transport_failure("Serial input", error);
    }
#endif

#if CONFIG_PLAYGATE_ENABLE_UDP
    error = input_transports_start_wifi_udp();
    if (error != ESP_OK) {
        record_transport_failure("Wi-Fi/UDP input", error);
    }
#endif

#if CONFIG_PLAYGATE_ENABLE_WEB
    error = web_console_start();
    if (error != ESP_OK) {
        record_transport_failure("Web console", error);
    }
#endif

    error = input_transports_start_status_publisher();
    if (error != ESP_OK) {
        record_transport_failure("status publisher", error);
    }
}
