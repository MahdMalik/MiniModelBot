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

/* #include "protocol_examples_common.h" // Wi-Fi connectivity */
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
#include "driver/i2c.h"

#include "PrintFunctions.h"
#include "motors.h"
#include "imu.h"
#include "camera.h"
#include "model.h"

#define PORT					30000
#define KEEPALIVE_IDLE			CONFIG_KEEPALIVE_IDLE
#define KEEPALIVE_INTERVAL		CONFIG_KEEPALIVE_INTERVAL
#define KEEPALIVE_COUNT			CONFIG_KEEPALIVE_COUNT

#define BLINK_GPIO GPIO_NUM_48
#define BLINK_PERIOD 1000


#define FADE_RESOLUTION			10

#include "headless_model.h"
#include "headless_model_data.h"

static uint8_t s_led_state = 0;
bool usingModel = true;

//just quickly putting the on-chip LED to high
void doBlink()
{
	gpio_reset_pin(BLINK_GPIO);
	gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
	s_led_state = 1;
	gpio_set_level(BLINK_GPIO, s_led_state);
}

extern "C" void app_main(void) {

	vTaskDelay(pdMS_TO_TICKS(5000));

    sensorSetup();
	cameraInit();
	if(usingModel)
	{
		connectHeadlessModel(g_model, g_model_len);
		setupModel();
	}
    ledc_setup();
	// doBlink();


	//should retry if not ready, 
    if(!isBmiReady || gotError || modelSetupFailed)
    {
        return;
    }
	
	// currentDirection[0] = 2;
	// currentDirection[1] = -2;
	// move(false);
	// for(int i = 2; i < 100; i++)
	// {
	// 	currentDirection[0] = i;
	// 	currentDirection[1] = -i;
	// 	move(false);
	// 	vTaskDelay(pdMS_TO_TICKS(50));
	// }

	IMUData newData = getSensorData();

    for (int i = 0; i < 100; i++) {
		vTaskDelay(pdMS_TO_TICKS(100)); // 100ms * 100 = 10 seconds
	}
	
	ESP_LOGI("INFO", "it worked out!");

    // Just launch the task and let it run
	ESP_LOGI("INFO", "Hopefully, something happened to the model");
    
    // app_main can now just chill or handle other things (like WiFi/HTTP)
    while(1) { vTaskDelay(pdMS_TO_TICKS(1000)); 
		if (usingModel)
		{
			modelCall();

			// Uncomment when you have a label source (button, serial, MQTT, etc.)
			modelLearn(0);
		}
	}
}