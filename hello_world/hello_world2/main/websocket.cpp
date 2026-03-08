// websocket.cpp — ESP-IDF WebSocket Server for ESP32-S3
// Connects to WiFi, prints IP, and listens for WebSocket frames from Python client.
//
// HOW TO USE:
//   1. Flash this to your ESP32-S3.
//   2. Open the serial monitor — it will print the IP address once connected.
//   3. In your Python script, connect to:  ws://<IP_SHOWN>:8765/ws

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"

// ─── CONFIG ───────────────────────────────────────────────
#define WIFI_SSID       "Galaxy S20 5G 74b1"
#define WIFI_PASSWORD   "helloooo"
#define WS_PORT          8765
#define WIFI_MAX_RETRY   10
// ──────────────────────────────────────────────────────────

static const char *TAG_WIFI = "WiFi";
static const char *TAG_WS   = "WebSocket";

// FreeRTOS event bits for WiFi state
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;

// ── Inference callback — set via ws_set_inference_callback() ─
// Default stub just logs the frame size until a real callback is registered.
typedef void (*ws_inference_cb_t)(const uint8_t *data, size_t data_len);

static ws_inference_cb_t s_inference_cb = NULL;

extern "C" void ws_set_inference_callback(ws_inference_cb_t cb)
{
    s_inference_cb = cb;
    ESP_LOGI("CV", "Inference callback %s.", cb ? "registered" : "cleared (using stub)");
}

static void run_inference(const uint8_t *data, size_t data_len)
{
    if (s_inference_cb) {
        s_inference_cb(data, data_len);
    } else {
        // Built-in stub — replace by calling ws_set_inference_callback()
        ESP_LOGI("CV", "Frame received: %u bytes (no inference callback registered).",
                 (unsigned int)data_len);
    }
}
// ──────────────────────────────────────────────────────────


// ─── WiFi Event Handler ───────────────────────────────────
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG_WIFI, "WiFi driver started — attempting to connect to \"%s\"...", WIFI_SSID);
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG_WIFI, "Associated with AP \"%s\" — waiting for IP address...", WIFI_SSID);

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        if (s_retry_count < WIFI_MAX_RETRY) {
            s_retry_count++;
            ESP_LOGW(TAG_WIFI, "Disconnected (reason %d). Retry %d/%d...",
                     disc->reason, s_retry_count, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG_WIFI, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
            ESP_LOGE(TAG_WIFI, "  WIFI FAILED after %d attempts.", WIFI_MAX_RETRY);
            ESP_LOGE(TAG_WIFI, "  Check SSID / password and reset board.");
            ESP_LOGE(TAG_WIFI, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        s_retry_count = 0;

        ESP_LOGI(TAG_WIFI, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        ESP_LOGI(TAG_WIFI, "  WiFi CONNECTED successfully!");
        ESP_LOGI(TAG_WIFI, "  SSID      : %s", WIFI_SSID);
        ESP_LOGI(TAG_WIFI, "  IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG_WIFI, "  Netmask   : " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG_WIFI, "  Gateway   : " IPSTR, IP2STR(&event->ip_info.gw));
        ESP_LOGI(TAG_WIFI, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        ESP_LOGI(TAG_WIFI, "  Python client: ws://" IPSTR ":%d/ws", IP2STR(&event->ip_info.ip), WS_PORT);
        ESP_LOGI(TAG_WIFI, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// ─── WiFi Initialisation ─────────────────────────────────
static bool wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {};
    memcpy(wifi_config.sta.ssid,     WIFI_SSID,     strlen(WIFI_SSID));
    memcpy(wifi_config.sta.password, WIFI_PASSWORD, strlen(WIFI_PASSWORD));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG_WIFI, "Waiting for WiFi connection (timeout: never)...");

    // Block until connected or all retries exhausted
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG_WIFI, "WiFi init complete — connection established.");
        return true;
    } else {
        ESP_LOGE(TAG_WIFI, "WiFi init complete — connection FAILED.");
        return false;
    }
}


// ─── WebSocket Frame Handler ─────────────────────────────
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // This is the initial WebSocket upgrade handshake
        ESP_LOGI(TAG_WS, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        ESP_LOGI(TAG_WS, "  New WebSocket client connected!");
        ESP_LOGI(TAG_WS, "  Sending READY signal to Python client...");
        ESP_LOGI(TAG_WS, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

        httpd_ws_frame_t ready_frame = {};
        ready_frame.type    = HTTPD_WS_TYPE_TEXT;
        ready_frame.payload = (uint8_t *)"READY";
        ready_frame.len     = 5;
        httpd_ws_send_frame(req, &ready_frame);
        return ESP_OK;
    }

    // Peek to get the frame length without consuming data
    httpd_ws_frame_t ws_pkt = {};
    ws_pkt.type = HTTPD_WS_TYPE_BINARY;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_WS, "Failed to get WebSocket frame length: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ws_pkt.len == 0) {
        return ESP_OK; // empty frame — ignore
    }

    // Allocate buffer and read the full payload
    uint8_t *buf = (uint8_t *)calloc(1, ws_pkt.len + 1);
    if (!buf) {
        ESP_LOGE(TAG_WS, "Out of memory! Could not allocate %u bytes for frame buffer.",
                 (unsigned int)ws_pkt.len);
        return ESP_ERR_NO_MEM;
    }
    ws_pkt.payload = buf;

    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_WS, "Failed to receive WebSocket frame payload: %s", esp_err_to_name(ret));
        free(buf);
        return ret;
    }

    // Handle by frame type
    if (ws_pkt.type == HTTPD_WS_TYPE_BINARY) {
        // Binary frame = camera frame from Python — run ML inference
        run_inference(ws_pkt.payload, ws_pkt.len);

        // ACK so the Python client knows to send the next frame
        httpd_ws_frame_t ack_frame = {};
        ack_frame.type    = HTTPD_WS_TYPE_TEXT;
        ack_frame.payload = (uint8_t *)"ACK";
        ack_frame.len     = 3;
        esp_err_t ack_ret = httpd_ws_send_frame(req, &ack_frame);
        if (ack_ret != ESP_OK) {
            ESP_LOGW(TAG_WS, "ACK send failed: %s", esp_err_to_name(ack_ret));
        }

    } else if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        ESP_LOGI(TAG_WS, "Text message received: \"%s\"", (char *)ws_pkt.payload);

    } else if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
        // Respond to ping with pong (keep-alive)
        httpd_ws_frame_t pong = {};
        pong.type    = HTTPD_WS_TYPE_PONG;
        pong.payload = ws_pkt.payload;
        pong.len     = ws_pkt.len;
        httpd_ws_send_frame(req, &pong);
        ESP_LOGD(TAG_WS, "PING received — PONG sent.");

    } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGW(TAG_WS, "Client sent CLOSE frame — connection closing.");

    } else {
        ESP_LOGW(TAG_WS, "Unknown WebSocket frame type: %d", ws_pkt.type);
    }

    free(buf);
    return ESP_OK;
}

// ─── Start WebSocket / HTTP Server ───────────────────────
static httpd_handle_t start_ws_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port    = WS_PORT;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);

    if (err != ESP_OK) {
        ESP_LOGE(TAG_WS, "Failed to start HTTP/WebSocket server: %s", esp_err_to_name(err));
        return NULL;
    }

    // Register the /ws endpoint as a WebSocket handler
    httpd_uri_t ws_uri = {};
    ws_uri.uri          = "/ws";
    ws_uri.method       = HTTP_GET;
    ws_uri.handler      = ws_handler;
    ws_uri.is_websocket = true;

    esp_err_t reg_err = httpd_register_uri_handler(server, &ws_uri);
    if (reg_err != ESP_OK) {
        ESP_LOGE(TAG_WS, "Failed to register /ws URI handler: %s", esp_err_to_name(reg_err));
        httpd_stop(server);
        return NULL;
    }

    ESP_LOGI(TAG_WS, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    ESP_LOGI(TAG_WS, "  WebSocket server READY on port %d", WS_PORT);
    ESP_LOGI(TAG_WS, "  Endpoint: ws://<IP shown above>:%d/ws", WS_PORT);
    ESP_LOGI(TAG_WS, "  Waiting for Python client to connect...");
    ESP_LOGI(TAG_WS, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

    return server;
}


// ─── Forward declaration from hello_world_main.c ─────────
// hw_init() sets up UART, prints chip info, and registers the
// ML inference callback before the network comes up.
extern "C" void hw_init(void);

// ─── Entry Point ─────────────────────────────────────────
extern "C" void app_main(void)
{
    // 1. Hardware init (UART, chip info, register inference callback)
    hw_init();

    // 2. NVS is required by the WiFi driver
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG_WIFI, "NVS partition worn/version mismatch — erasing and re-initialising...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG_WIFI, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    ESP_LOGI(TAG_WIFI, "  ESP32-S3 WebSocket CV Node — starting up");
    ESP_LOGI(TAG_WIFI, "  Target SSID: %s", WIFI_SSID);
    ESP_LOGI(TAG_WIFI, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

    // 3. Connect to WiFi — blocks here until success or max retries
    bool connected = wifi_init_sta();
    if (!connected) {
        ESP_LOGE(TAG_WIFI, "Halting: no WiFi connection. Reset the board to retry.");
        return;
    }

    // 4. Start the WebSocket server
    httpd_handle_t server = start_ws_server();
    if (server == NULL) {
        ESP_LOGE(TAG_WS, "Halting: WebSocket server failed to start.");
        return;
    }

    // httpd runs in its own FreeRTOS task — app_main can return.
    ESP_LOGI("Main", "System ready. Waiting for Python client on ws://<IP>:%d/ws", WS_PORT);
}