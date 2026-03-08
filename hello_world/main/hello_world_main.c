/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "websocket.h"

// ─── WiFi credentials ────────────────────────────────────────────────────────
#define WIFI_SSID      "Galaxy S20 5G 74b1"
#define WIFI_PASSWORD  "helloooo"
#define MAX_RETRY      5
// ─────────────────────────────────────────────────────────────────────────────

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "WiFi";
static int retry_count = 0;

// ── WiFi event handler ────────────────────────────────────────────────────────
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "[WiFi] STA started — attempting to connect…");
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_count < MAX_RETRY) {
            retry_count++;
            ESP_LOGW(TAG, "[WiFi] Disconnected. Retry %d/%d…", retry_count, MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "[WiFi] Could not connect after %d attempts. Giving up.", MAX_RETRY);
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "[WiFi] ✓ Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// ── Connect to WiFi (blocks until connected or failed) ───────────────────────
static bool wifi_init_sta(void)
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
                                                        &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "[WiFi] Connecting to SSID: %s", WIFI_SSID);

    // Block until connected or all retries exhausted.
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    // Clean up event handlers — the WebSocket server registers its own if needed.
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(wifi_event_group);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "[WiFi] Connection successful!");
        return true;
    }

    ESP_LOGE(TAG, "[WiFi] Connection FAILED.");
    return false;
}

// ── Entry point ───────────────────────────────────────────────────────────────
void app_main(void)
{
    ESP_LOGI(TAG, "══════════════════════════════════════════");
    ESP_LOGI(TAG, "  ESP32-S3 WebSocket CV Server starting…");
    ESP_LOGI(TAG, "══════════════════════════════════════════");

    // NVS is required by the WiFi driver.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "[NVS] Erasing NVS and reinitialising…");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "[NVS] Initialised.");

    // Connect to WiFi. If it fails there is nothing more we can do.
    if (!wifi_init_sta()) {
        ESP_LOGE(TAG, "Halting: no WiFi connection.");
        // Sit here rather than restarting so the error is visible on the monitor.
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    // WiFi is up — start the WebSocket server.
    // The IP address and connection URL are printed inside this call.
    websocket_server_start();

    // app_main must not return (that would delete the main task).
    // The HTTP server runs its own tasks internally, so we just idle here.
    ESP_LOGI(TAG, "[Main] Server running. Waiting for Python client…");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}