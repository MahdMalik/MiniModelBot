#include "rom/ets_sys.h"
#include "esp_log.h"
#include "esp_err.h"

#include "freertos/projdefs.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "hal/ledc_types.h"

#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "soc/clk_tree_defs.h"
#include <sys/socket.h>		// Sockets
#include <unistd.h>			// Close
#include <netdb.h>			// gethostbyname
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "driver/timer.h"
#include <fcntl.h>
#include "freertos/semphr.h"
#include "bmi270.hpp"
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_camera.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "PrintFunctions.h"

//for current direction, ti's between -100 and 100. negative numbers mean reverse motor,
//and -100 reverses and greater speed than say -10, and 100 goes at faster spee than say
//10. We will later map this to the proper duty values
extern float currentDirection[2];
//stores the direction values we are currently at and our starting values
extern int8_t currentTargets[2];
extern float startTargets[2];

float getRawDutyFromPercent(float duty);
uint32_t getRawDutyFromBaseDirection(float direction);
float convertPulseWidthToPercentDuty(int pulseWidth);
void actuallyUpdateDuties(ledc_channel_t channel, float chosenDirection);
void move(bool startup);
void ledc_setup();