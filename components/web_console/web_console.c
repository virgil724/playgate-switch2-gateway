#include "web_console.h"

#include "sdkconfig.h"

#if CONFIG_PLAYGATE_ENABLE_WEB

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "controller_core.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "gateway_status.h"

#define WEB_STATUS_SIZE 20u    /* gateway_status_encode output length */
#define WEB_INPUT_SIZE 17u     /* PLAYGATE_INPUT_PAYLOAD_SIZE */
#define WEB_STATUS_PROBE 0xf0u /* matches udp_input.c UDP_STATUS_PROBE */
#define WEB_WS_SCRATCH 32u

extern const uint8_t _binary_gamepad_html_start[] asm("_binary_gamepad_html_start");
extern const uint8_t _binary_gamepad_html_end[] asm("_binary_gamepad_html_end");

static httpd_handle_t s_server;

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    const ssize_t length = (ssize_t)(_binary_gamepad_html_end - _binary_gamepad_html_start);
    const esp_err_t err = httpd_resp_send(req,
                                         (const char *)_binary_gamepad_html_start,
                                         length);
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t ws_send_status(httpd_req_t *req)
{
    uint8_t status[WEB_STATUS_SIZE];
    gateway_status_encode(esp_timer_get_time(), status);

    httpd_ws_frame_t out = {
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = status,
        .len = WEB_STATUS_SIZE,
    };
    return httpd_ws_send_frame(req, &out);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_BINARY,
    };
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK) {
        return ESP_OK;
    }
    if (frame.len == 0u || frame.len > WEB_WS_SCRATCH) {
        return ESP_OK;
    }

    uint8_t payload[WEB_WS_SCRATCH];
    frame.payload = payload;
    if (httpd_ws_recv_frame(req, &frame, frame.len) != ESP_OK) {
        return ESP_OK;
    }
    if (frame.type != HTTPD_WS_TYPE_BINARY) {
        return ESP_OK;
    }

    /* A 17-byte frame is controller input; a lone 0xF0 byte is a status poll. */
    if (frame.len == WEB_INPUT_SIZE) {
        (void)input_core_submit(INPUT_SOURCE_UDP,
                                payload,
                                WEB_INPUT_SIZE,
                                esp_timer_get_time());
    } else if (frame.len != 1u || payload[0] != WEB_STATUS_PROBE) {
        return ESP_OK;
    }

    return ws_send_status(req);
}

esp_err_t web_console_start(void)
{
    if (s_server != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        s_server = NULL;
        return err;
    }

    static const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get,
    };
    static const httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
    };

    err = httpd_register_uri_handler(s_server, &root_uri);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(s_server, &ws_uri);
    }
    if (err != ESP_OK) {
        (void)httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    return ESP_OK;
}

#else /* !CONFIG_PLAYGATE_ENABLE_WEB */

esp_err_t web_console_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
