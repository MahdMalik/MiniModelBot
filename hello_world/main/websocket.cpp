#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"

// ─── CONFIG ───────────────────────────────────────────────
#define WIFI_SSID       "Galaxy S20 5G 74b1"
#define WIFI_PASSWORD   "helloooo"
#define WS_PORT         8765
#define MAX_RETRY       5
// ──────────────────────────────────────────────────────────

static const char *TAG = "WS_SERVER";
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int retry_count = 0;
static httpd_handle_t server = NULL;

// ── Stub: replace with your actual model inference call ───
static void run_inference(const uint8_t *data, size_t data_len)
{
    ESP_LOGI(TAG, "[CV] Received frame: %u bytes", (unsigned)data_len);
}

// ─── WiFi Event Handler ───────────────────────────────────
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_count < MAX_RETRY) {
            esp_wifi_connect();
            retry_count++;
            ESP_LOGI(TAG, "Retrying WiFi... (%d/%d)", retry_count, MAX_RETRY);
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "[WiFi] Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// ─── WiFi Init ────────────────────────────────────────────
static void wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));
    memcpy(wifi_config.sta.ssid,     WIFI_SSID,     strlen(WIFI_SSID));
    memcpy(wifi_config.sta.password, WIFI_PASSWORD, strlen(WIFI_PASSWORD));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to WiFi");
    }
    else{
        ESP_LOGE(TAG, "Connected to WiFi...");
    }
}

// ─── WebSocket Handler ────────────────────────────────────
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "[WS] Client connected!");
        httpd_ws_frame_t ready_frame;
        memset(&ready_frame, 0, sizeof(ready_frame));
        ready_frame.type    = HTTPD_WS_TYPE_TEXT;
        ready_frame.payload = (uint8_t *)"READY";
        ready_frame.len     = 5;
        httpd_ws_send_frame(req, &ready_frame);
        return ESP_OK;
    }

    // First pass: get frame length only
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) return ret;
    if (ws_pkt.len == 0) return ESP_OK;

    // Allocate buffer and receive actual data
    uint8_t *buf = (uint8_t *)calloc(1, ws_pkt.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;

    ws_pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        free(buf);
        return ret;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_BINARY) {
        run_inference(ws_pkt.payload, ws_pkt.len);

        // Send ACK
        httpd_ws_frame_t ack_frame;
        memset(&ack_frame, 0, sizeof(ack_frame));
        ack_frame.type    = HTTPD_WS_TYPE_TEXT;
        ack_frame.payload = (uint8_t *)"ACK";
        ack_frame.len     = 3;
        httpd_ws_send_frame(req, &ack_frame);

    } else if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        ESP_LOGI(TAG, "[WS] Text: %s", ws_pkt.payload);
    } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "[WS] Client disconnected.");
    }

    free(buf);
    return ESP_OK;
}

static const httpd_uri_t ws_uri = {
    "/",
    HTTP_GET,
    ws_handler,
    NULL,
    true,       // is_websocket
    false,      // handle_ws_control_frames
    NULL        // supported_subprotocol
};

// ─── Start WebSocket Server ───────────────────────────────
static void start_ws_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WS_PORT;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &ws_uri);
        ESP_LOGI(TAG, "[WS] Server started on port %d", WS_PORT);
    } else {
        ESP_LOGI(TAG, "[WS] ERROR: Server failed to start!");
    }
}

// ─── Main ─────────────────────────────────────────────────
extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    
    wifi_init_sta();
    start_ws_server();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}