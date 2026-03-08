#pragma once
// websocket.h — Public interface for the ESP32-S3 WebSocket / WiFi module
//
// Include this header in any other .c or .cpp file that needs to:
//   - Start the WiFi + WebSocket server
//   - Query current connection status
//   - Send frames back to the connected Python client
//   - Hook in the ML inference callback
//
// Typical usage in hello_world_main.c:
//   #include "websocket.h"
//   ...
//   ws_node_start();               // connect WiFi + launch server
//   ws_set_inference_callback(my_inference_fn);

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

// ─── Configuration ────────────────────────────────────────
// Override these here (or via idf.py menuconfig / sdkconfig) to
// change network settings without editing websocket.cpp.

#ifndef WIFI_SSID
#define WIFI_SSID        "Galaxy S20 5G 74b1"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD    "helloooo"
#endif

// Port the WebSocket server listens on.
// Python client connects to:  ws://<IP>:<WS_PORT>/ws
#ifndef WS_PORT
#define WS_PORT          8765
#endif

// How many times the WiFi driver retries before giving up.
#ifndef WIFI_MAX_RETRY
#define WIFI_MAX_RETRY   10
#endif

// WebSocket endpoint path (change if your Python client uses a different path)
#ifndef WS_ENDPOINT
#define WS_ENDPOINT      "/ws"
#endif
// ──────────────────────────────────────────────────────────


// ─── WiFi Connection State ────────────────────────────────

/**
 * @brief Possible states of the WiFi / WebSocket node.
 */
typedef enum {
    WS_NODE_IDLE        = 0,   ///< Not yet initialised
    WS_NODE_CONNECTING,        ///< Attempting WiFi connection
    WS_NODE_WIFI_OK,           ///< WiFi connected, server not yet started
    WS_NODE_SERVER_READY,      ///< Server running, no client connected
    WS_NODE_CLIENT_CONNECTED,  ///< Python client is connected
    WS_NODE_WIFI_FAILED,       ///< All retries exhausted
    WS_NODE_SERVER_FAILED,     ///< httpd failed to start
} ws_node_state_t;

/**
 * @brief Return the current node state.
 *
 * Safe to call from any task — reads an atomic value.
 */
ws_node_state_t ws_node_get_state(void);

/**
 * @brief Return a human-readable string for a node state.
 *
 * Useful for log messages:
 *   ESP_LOGI("Main", "State: %s", ws_node_state_str(ws_node_get_state()));
 */
const char *ws_node_state_str(ws_node_state_t state);


// ─── Lifecycle ────────────────────────────────────────────

/**
 * @brief Initialise NVS, connect to WiFi, and start the WebSocket server.
 *
 * Blocks until WiFi is either established or all retries are exhausted.
 * The WebSocket server is then launched in its own FreeRTOS task.
 *
 * Call this once from app_main() (or equivalent) before doing anything
 * that requires network access.
 *
 * @return ESP_OK            on success (server running, awaiting Python client)
 * @return ESP_FAIL          if WiFi or server startup failed
 */
esp_err_t ws_node_start(void);

/**
 * @brief Stop the WebSocket server and disconnect WiFi.
 *
 * Cleans up the httpd handle. Safe to call even if ws_node_start()
 * was never called or returned an error.
 */
void ws_node_stop(void);


// ─── Sending Frames to the Python Client ─────────────────

/**
 * @brief Send a text frame to the currently connected Python client.
 *
 * @param text  NULL-terminated string to send.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if no client is connected.
 */
esp_err_t ws_send_text(const char *text);

/**
 * @brief Send a binary frame to the currently connected Python client.
 *
 * @param data      Pointer to raw bytes.
 * @param data_len  Number of bytes to send.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if no client is connected.
 */
esp_err_t ws_send_binary(const uint8_t *data, size_t data_len);


// ─── Inference Callback Hook ──────────────────────────────

/**
 * @brief Signature of the function called when a binary frame (camera frame)
 *        is received from the Python client.
 *
 * @param data      Pointer to raw frame bytes. Valid only for the duration
 *                  of the callback — copy if you need it later.
 * @param data_len  Number of bytes in the frame.
 */
typedef void (*ws_inference_cb_t)(const uint8_t *data, size_t data_len);

/**
 * @brief Register a callback to be invoked on every incoming binary frame.
 *
 * Replaces the built-in stub (which just logs the frame size).
 * Call before ws_node_start(), or at any time — the new callback takes
 * effect on the next received frame.
 *
 * Example:
 *   static void my_model_fn(const uint8_t *data, size_t len) {
 *       cv_model_set_input(data, len);
 *       cv_model_run();
 *       int result = cv_model_get_output();
 *       ws_send_text(result == 1 ? "PERSON" : "NO_PERSON");
 *   }
 *   ws_set_inference_callback(my_model_fn);
 *
 * @param cb  Function pointer, or NULL to restore the built-in stub.
 */
void ws_set_inference_callback(ws_inference_cb_t cb);


// ─── Connection Info ──────────────────────────────────────

/**
 * @brief Retrieve the ESP32's current IPv4 address as a string.
 *
 * Writes up to buf_len bytes (including NUL terminator) into buf.
 * Returns false and writes an empty string if not connected.
 *
 * Example:
 *   char ip[16];
 *   if (ws_get_ip_str(ip, sizeof(ip))) {
 *       ESP_LOGI("Main", "Connect to: ws://%s:%d/ws", ip, WS_PORT);
 *   }
 *
 * @param buf      Output buffer (minimum 16 bytes for a full IPv4 string).
 * @param buf_len  Size of buf.
 * @return true if an IP address is available, false otherwise.
 */
bool ws_get_ip_str(char *buf, size_t buf_len);

/**
 * @brief Return true if a Python WebSocket client is currently connected.
 */
bool ws_client_is_connected(void);

#ifdef __cplusplus
}
#endif