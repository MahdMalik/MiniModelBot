#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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
#include <cstring>


void SetSendingImage(bool setter);

void SetCanInference(bool setter);

void CustomPrint(const char* logType, const char* data, ...);

bool ReadForReadiness();

void CustomWrite(size_t number);

void CustomWrite(float number);

void CustomWrite(uint8_t* buf);