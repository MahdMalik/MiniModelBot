idf_component_register(SRCS "main.cpp" "PrintFunctions.cpp" "model_data.h" "mainModel.cpp"
                    PRIV_REQUIRES spi_flash
                    INCLUDE_DIRS "."
                    REQUIRES esp32-camera esp-tflite-micro espp__bmi270 espp__base_peripheral # Required by espp
                    )