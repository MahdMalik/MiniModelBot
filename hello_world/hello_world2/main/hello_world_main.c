/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

// hello_world_main.c
// Chip info, UART setup, and ML model entry point.
// WiFi + WebSocket are handled entirely by websocket.cpp — do NOT call
// wifi_init_sta() or define app_main() here.

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "driver/uart.h"
#include "esp_log.h"

#include "websocket.h"   // ws_node_start(), ws_set_inference_callback(), ws_send_text()

static const char *TAG = "Main";

// ─── UART Setup ───────────────────────────────────────────
static void uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_NUM_1, 1024, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_1, &uart_config);
    ESP_LOGI(TAG, "UART1 initialised at 115200 baud.");
}

// ─── Chip Info ────────────────────────────────────────────
static void print_chip_info(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);

    printf("Chip   : %s\n", CONFIG_IDF_TARGET);
    printf("Cores  : %d CPU core(s)\n", chip_info.cores);
    printf("Features: %s%s%s%s\n",
           (chip_info.features & CHIP_FEATURE_WIFI_BGN)    ? "WiFi "        : "",
           (chip_info.features & CHIP_FEATURE_BT)          ? "BT "          : "",
           (chip_info.features & CHIP_FEATURE_BLE)         ? "BLE "         : "",
           (chip_info.features & CHIP_FEATURE_IEEE802154)  ? "802.15.4 "    : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    printf("Silicon: v%d.%d\n", major_rev, minor_rev);

    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        printf("Flash  : %" PRIu32 " MB %s\n",
               flash_size / (uint32_t)(1024 * 1024),
               (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
    } else {
        printf("Flash  : (size unknown)\n");
    }

    printf("Free heap: %" PRIu32 " bytes\n", esp_get_minimum_free_heap_size());
}

// ─── ML Inference Callback ────────────────────────────────
// This is called by websocket.cpp every time a binary frame
// (camera frame) arrives from the Python client.
// Replace the body with your actual model API calls.
static void on_frame_received(const uint8_t *data, size_t data_len)
{
    ESP_LOGI(TAG, "Frame received: %u bytes — running inference...", (unsigned int)data_len);

    // TODO: plug in your TFLite model here, e.g.:
      cv_model_set_input(data, data_len);
      cv_model_run();
      int result = cv_model_get_output();
      ws_send_text(result == 1 ? "PERSON" : "NO_PERSON");
}

// ─── Entry Point ─────────────────────────────────────────
// app_main is defined in websocket.cpp. It calls ws_node_start()
// and the WebSocket server internally. This function is called
// from there once the network is ready.
//
// If you need custom startup logic before WiFi, add it to
// websocket.cpp's app_main() directly, or restructure so that
// websocket.cpp calls a hw_init() function you define here.

void hw_init(void)
{
    print_chip_info();
    uart_init();

    // Register the inference callback so websocket.cpp forwards
    // incoming camera frames directly to our model function.
    ws_set_inference_callback(on_frame_received);

    ESP_LOGI(TAG, "Hardware init complete. Handing off to WebSocket node...");
}