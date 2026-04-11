idf_component_register(SRCS "main.cpp" "src/camera.cpp" "src/imu.cpp" "src/model.cpp" "src/motors.cpp" "src/PrintFunctions.cpp" "src/custom_layer.cpp"
                    PRIV_REQUIRES spi_flash
                    INCLUDE_DIRS "include"
                    REQUIRES esp32-camera esp-tflite-micro espp__bmi270 espp__base_peripheral nvs_flash# Required by espp
                    )