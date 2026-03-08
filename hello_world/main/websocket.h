#pragma once
// websocket.h — public API for the ESP-IDF WebSocket server

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the WebSocket server.
 *
 * Must be called AFTER wifi_init_sta() succeeds so that the device
 * already has a valid IP address.
 *
 * The server listens on port 8765.  The Python script on your laptop
 * should connect to:
 *
 *   ws://<ESP32-S3 IP>:8765/ws
 *
 * The IP address is printed to the serial monitor when this function runs.
 */
void websocket_server_start(void);

/**
 * @brief Stop the WebSocket server and free resources.
 */
void websocket_server_stop(void);

#ifdef __cplusplus
}
#endif