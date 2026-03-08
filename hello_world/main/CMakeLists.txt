idf_component_register(
    SRCS "hello_world_main.c" "model_weights.cpp" "websocket.cpp"
    PRIV_REQUIRES spi_flash
    INCLUDE_DIRS "."
    REQUIRES esp32-camera esp-tflite-micro esp_wifi esp_event esp_netif nvs_flash driver esp_system esp_http_server
)
