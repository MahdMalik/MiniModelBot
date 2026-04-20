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
#include <sys/socket.h> // Sockets
#include <unistd.h>     // Close
#include <netdb.h>      // gethostbyname
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
#include <iostream>

#include "PrintFunctions.h"
#include "motors.h"
#include "imu.h"
#include "camera.h"
#include "model.h"
#include "my_littlefs.h"

#include "esp_camera.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
// #include "tensorflow/lite/system_setup.h"
#include "model_data.h"

#define PORT 30000
#define KEEPALIVE_IDLE CONFIG_KEEPALIVE_IDLE
#define KEEPALIVE_INTERVAL CONFIG_KEEPALIVE_INTERVAL
#define KEEPALIVE_COUNT CONFIG_KEEPALIVE_COUNT

#define BLINK_GPIO GPIO_NUM_48
#define BLINK_PERIOD 1000

#define FADE_RESOLUTION 10

#define CONFIDENCE_THRESHOLD 0.5f
#define TURN_HURST_MS 400
#define DRIVE_FORWARD_POWER 20
#define TURN_POWER 100

#include "headless_model.h"
#include "headless_model_data.h"

static uint8_t s_led_state = 0;
bool usingModel = true;

// returns 0 if traversible (velocity<.5) returns 1 if traversible
int getLabel()
{
    double velocity = getInstantVelocity();
    if (velocity <= 0.5)
    {
        return 0;
    }
    else
    {
        return 1;
    }
}

// static const unsigned char *const modelWeights =
//     _content_drive_MyDrive_ACMResearchDataset_model_model_cnn_int8_tflite;
static const int TENSOR_ARENA_SIZE = 190000;
static uint8_t tensorArena[TENSOR_ARENA_SIZE];
static tflite::MicroInterpreter *interpreter = nullptr;
static float inputScale = 0.0f;
static int32_t inputZeroPoint = 0;
static float outputScale = 0.0f;
static int32_t outputZeroPoint = 0;

// just quickly putting the on-chip LED to high
//TODO: make 
void doBlink()
{
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
    s_led_state = 1;
    gpio_set_level(BLINK_GPIO, s_led_state);
}

static void control_task(void *pvParameters)
{
    while (true)
    {
        modelCall();
		std::cout<<"Model was called!";
        modelLearn(getLabel()); // i moved it from app_main so it runs in the same task as inference
		std::cout<<"Continous learning was called label was "+ std::to_string(getLabel());

        camera_fb_t *frame = esp_camera_fb_get();
        if (frame == nullptr)
        {
            ESP_LOGE("CAMERA", "Camera capture failed");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        float traversableProb = getLastClass1Prob();
		std::cout<<"Last Class 1 prob "+ std::to_string(traversableProb);
        esp_camera_fb_return(frame);

        //checking if frame is intraversible
        if (traversableProb < CONFIDENCE_THRESHOLD)
        {
			turnRight();
            vTaskDelay(pdMS_TO_TICKS(200));
			ESP_LOGI("CONTROL", "traversable: %.2f", traversableProb);
            continue;
        }
        //checking if frame is traversible (equal to or above the confidence threshold)
        else if (traversableProb >= CONFIDENCE_THRESHOLD)
        {
            ESP_LOGI("CONTROL", "path is clear, driving forward");
			move();
			vTaskDelay(pdMS_TO_TICKS(200));
			ESP_LOGI("CONTROL", "path is clear, driving forward");
        }
    }
}

extern "C" void app_main(void)
{
	littleFSInit();
    vTaskDelay(pdMS_TO_TICKS(5000));

    sensorSetup();
    cameraInit();
    if (usingModel)
    {
        connectHeadlessModel(g_model, g_model_len);
        setupModel();
    }
    ledc_setup();
    // doBlink();

    if (!isBmiReady || gotError || modelSetupFailed)
    {
        return;
    }


    xTaskCreate(control_task, "control_task", 8192, NULL, 5, NULL); // modellearn() is in here now btw

    // while (1)
    // {
    //     vTaskDelay(pdMS_TO_TICKS(1000));
    // }
}

// // testing movement without camera
// extern "C" void app_main(void)
// {
//     vTaskDelay(pdMS_TO_TICKS(5000));

//     //sensorSetup();
//     //cameraInit();
//     ledc_setup();

//     /*if (!isBmiReady || gotError)
//     {
//         ESP_LOGE("MAIN", "Setup failed");
//         return;
//     }
//     */
//     ESP_LOGI("MAIN", "Starting motor test loop");

//     // ramp motor up to move forward
//     for (int i =0; i < 20; i++) {
//         vTaskDelay(pdMS_TO_TICKS(100));
//         currentDirection[0] = i;
//         currentDirection[1] = i;
//         ESP_LOGI("MAIN", "Setting power: %d", i);

//         move(false);
//     }

//     while (true)
//     {
//         vTaskDelay(pdMS_TO_TICKS(100));

//         // --- Stop briefly ---
//         currentDirection[0] = 0;
//         currentDirection[1] = 0;
//         move(false);
//         vTaskDelay(pdMS_TO_TICKS(300));

//         // Random rotate left or right
//         int turnDir = (esp_random() & 1) ? 1 : -1;
//         ESP_LOGI("MAIN", "Rotating %s...", turnDir == 1 ? "right" : "left");
//         currentDirection[0] = 100 * turnDir;
//         currentDirection[1] = 100 * -turnDir;
//         move(false);
//         vTaskDelay(pdMS_TO_TICKS(5000));

//         //Stop briefly before next forward movement
//         currentDirection[0] = 0;
//         currentDirection[1] = 0;
//         move(false);
//         vTaskDelay(pdMS_TO_TICKS(300));
//     }
// }

// //should retry if not ready,
// if(!isBmiReady || gotError || modelSetupFailed)
// {
//     return;
// }

// IMUData newData = getSensorData();

// 	ESP_LOGI("INFO", "it worked out!");

//     // Just launch the task and let it run
// 	ESP_LOGI("INFO", "Hopefully, something happened to the model");

//     // app_main can now just chill or handle other things (like WiFi/HTTP)
//     while(1) { vTaskDelay(pdMS_TO_TICKS(1000));
// 		if (usingModel)
// 		{
// 			modelCall();

// 			// Uncomment when you have a label source (button, serial, MQTT, etc.)
// 			modelLearn(getLabel());
// 		}
// 	}
// }