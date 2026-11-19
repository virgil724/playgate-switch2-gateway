#include "input_transports.h"

#include "sdkconfig.h"

#if CONFIG_PLAYGATE_ENABLE_UDP

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "controller_core.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway_status.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#define UDP_TASK_STACK_DEPTH 4096
#define UDP_TASK_PRIORITY 8
#define UDP_IDLE_WAIT_MS 20
#define UDP_RETRY_WAIT_MS 250
#define UDP_STATUS_PROBE 0xf0u

static StaticTask_t s_udp_task_tcb;
static StackType_t s_udp_task_stack[UDP_TASK_STACK_DEPTH];
static TaskHandle_t s_udp_task;
static bool s_wifi_connected;
static bool s_ap_mode;
static bool s_socket_rebuild_requested;
static portMUX_TYPE s_udp_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_netif_t *s_wifi_netif;
static esp_event_handler_instance_t s_wifi_event_instance;
static esp_event_handler_instance_t s_ip_event_instance;

static bool wifi_is_connected(void)
{
    portENTER_CRITICAL(&s_udp_lock);
    const bool connected = s_wifi_connected;
    portEXIT_CRITICAL(&s_udp_lock);
    return connected;
}

static void wifi_set_connected(bool connected)
{
    portENTER_CRITICAL(&s_udp_lock);
    s_wifi_connected = connected;
    portEXIT_CRITICAL(&s_udp_lock);
}

static void udp_request_socket_rebuild(void)
{
    portENTER_CRITICAL(&s_udp_lock);
    s_socket_rebuild_requested = true;
    portEXIT_CRITICAL(&s_udp_lock);
}

static bool udp_take_socket_rebuild_request(void)
{
    portENTER_CRITICAL(&s_udp_lock);
    const bool requested = s_socket_rebuild_requested;
    s_socket_rebuild_requested = false;
    portEXIT_CRITICAL(&s_udp_lock);
    return requested;
}


static void udp_task_wake(void)
{
    portENTER_CRITICAL(&s_udp_lock);
    TaskHandle_t task = s_udp_task;
    portEXIT_CRITICAL(&s_udp_lock);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
}

static void wifi_event_handler(void *argument,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)argument;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        (void)esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_set_connected(false);
        udp_request_socket_rebuild();
        gateway_status_set_wifi_connected(false);
        udp_task_wake();
        (void)esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_set_connected(true);
        gateway_status_set_wifi_connected(true);
        udp_task_wake();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        wifi_set_connected(true);
        gateway_status_set_wifi_connected(true);
        udp_task_wake();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        wifi_set_connected(false);
        udp_request_socket_rebuild();
        gateway_status_set_wifi_connected(false);
        udp_task_wake();
    }
}

static int udp_open_socket(void)
{
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        return -1;
    }

    const int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(sock);
        return -1;
    }

    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_PLAYGATE_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        close(sock);
        return -1;
    }

    return sock;
}

static bool udp_drain_socket(int sock)
{
    uint8_t packet[PLAYGATE_INPUT_PAYLOAD_SIZE + 1u];

    while (wifi_is_connected()) {
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        const ssize_t received = recvfrom(sock,
                                          packet,
                                          sizeof(packet),
                                          0,
                                          (struct sockaddr *)&peer,
                                          &peer_len);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        if ((size_t)received == PLAYGATE_INPUT_PAYLOAD_SIZE) {
            (void)input_core_submit(INPUT_SOURCE_UDP,
                                    packet,
                                    PLAYGATE_INPUT_PAYLOAD_SIZE,
                                    esp_timer_get_time());
        } else if (received == 1 && packet[0] == UDP_STATUS_PROBE) {
            uint8_t status[GATEWAY_STATUS_SIZE];
            gateway_status_encode(esp_timer_get_time(), status);
            (void)sendto(sock,
                         status,
                         sizeof(status),
                         0,
                         (const struct sockaddr *)&peer,
                         peer_len);
        }
    }

    return true;
}

static void udp_input_task(void *context)
{
    (void)context;
    int sock = -1;

    for (;;) {
        if (udp_take_socket_rebuild_request() && sock >= 0) {
            close(sock);
            sock = -1;
        }
        if (!wifi_is_connected()) {
            if (sock >= 0) {
                close(sock);
                sock = -1;
            }
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(UDP_IDLE_WAIT_MS));
            continue;
        }

        if (sock < 0) {
            sock = udp_open_socket();
            if (sock < 0) {
                (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(UDP_RETRY_WAIT_MS));
                continue;
            }
        }

        if (!udp_drain_socket(sock)) {
            close(sock);
            sock = -1;
            continue;
        }
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        struct timeval tv = {
            .tv_sec = 0,
            .tv_usec = UDP_IDLE_WAIT_MS * 1000,
        };
        const int ready = select(sock + 1, &readfds, NULL, NULL, &tv);
        if (ready < 0 && errno != EINTR) {
            close(sock);
            sock = -1;
        }
    }
}

static size_t build_ap_ssid(char *out, size_t capacity)
{
    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    return (size_t)snprintf(out,
                            capacity,
                            "%s-%02X%02X",
                            CONFIG_PLAYGATE_AP_SSID_PREFIX,
                            mac[4],
                            mac[5]);
}

static void wifi_cleanup(void)
{
    if (s_ip_event_instance != NULL) {
        (void)esp_event_handler_instance_unregister(IP_EVENT,
                                                    IP_EVENT_STA_GOT_IP,
                                                    s_ip_event_instance);
        s_ip_event_instance = NULL;
    }
    if (s_wifi_event_instance != NULL) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT,
                                                    ESP_EVENT_ANY_ID,
                                                    s_wifi_event_instance);
        s_wifi_event_instance = NULL;
    }
    wifi_set_connected(false);
    gateway_status_set_wifi_connected(false);
    (void)esp_wifi_stop();
    (void)esp_wifi_deinit();
    if (s_wifi_netif != NULL) {
        esp_netif_destroy_default_wifi(s_wifi_netif);
        s_wifi_netif = NULL;
    }
}

esp_err_t input_transports_start_wifi_udp(void)
{
    if (s_udp_task != NULL || s_wifi_netif != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ap_mode = CONFIG_PLAYGATE_WIFI_SSID[0] == '\0';

    wifi_config_t wifi_config = {0};
    if (!s_ap_mode) {
        const size_t ssid_len = strlen(CONFIG_PLAYGATE_WIFI_SSID);
        const size_t password_len = strlen(CONFIG_PLAYGATE_WIFI_PASSWORD);
        if (ssid_len > sizeof(wifi_config.sta.ssid) ||
            password_len > sizeof(wifi_config.sta.password)) {
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(wifi_config.sta.ssid, CONFIG_PLAYGATE_WIFI_SSID, ssid_len);
        memcpy(wifi_config.sta.password, CONFIG_PLAYGATE_WIFI_PASSWORD, password_len);
    } else {
        char ssid[sizeof(wifi_config.ap.ssid) + 1u];
        size_t ssid_len = build_ap_ssid(ssid, sizeof(ssid));
        if (ssid_len > sizeof(wifi_config.ap.ssid)) {
            ssid_len = sizeof(wifi_config.ap.ssid);
        }
        memcpy(wifi_config.ap.ssid, ssid, ssid_len);
        wifi_config.ap.ssid_len = (uint8_t)ssid_len;
        wifi_config.ap.channel = 1;
        wifi_config.ap.max_connection = 4;

        const size_t password_len = strlen(CONFIG_PLAYGATE_AP_PASSWORD);
        if (password_len < 8u) {
            wifi_config.ap.authmode = WIFI_AUTH_OPEN;
        } else if (password_len > sizeof(wifi_config.ap.password)) {
            return ESP_ERR_INVALID_ARG;
        } else {
            memcpy(wifi_config.ap.password, CONFIG_PLAYGATE_AP_PASSWORD, password_len);
            wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        }
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_wifi_netif = s_ap_mode ? esp_netif_create_default_wifi_ap()
                             : esp_netif_create_default_wifi_sta();
    if (s_wifi_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) {
        esp_netif_destroy_default_wifi(s_wifi_netif);
        s_wifi_netif = NULL;
        return err;
    }
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(WIFI_EVENT,
                                                  ESP_EVENT_ANY_ID,
                                                  wifi_event_handler,
                                                  NULL,
                                                  &s_wifi_event_instance);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT,
                                                  IP_EVENT_STA_GOT_IP,
                                                  wifi_event_handler,
                                                  NULL,
                                                  &s_ip_event_instance);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(s_ap_mode ? WIFI_MODE_AP : WIFI_MODE_STA);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_config(s_ap_mode ? WIFI_IF_AP : WIFI_IF_STA, &wifi_config);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_ps(WIFI_PS_NONE);
    }
    if (err != ESP_OK) {
        wifi_cleanup();
        return err;
    }

    TaskHandle_t task = xTaskCreateStatic(udp_input_task,
                                          "playgate_udp",
                                          UDP_TASK_STACK_DEPTH,
                                          NULL,
                                          UDP_TASK_PRIORITY,
                                          s_udp_task_stack,
                                          &s_udp_task_tcb);
    if (task == NULL) {
        wifi_cleanup();
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_udp_lock);
    s_udp_task = task;
    portEXIT_CRITICAL(&s_udp_lock);

    udp_task_wake();
    return ESP_OK;
}

#else /* !CONFIG_PLAYGATE_ENABLE_UDP */

esp_err_t input_transports_start_wifi_udp(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
