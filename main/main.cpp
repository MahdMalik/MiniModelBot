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
#include "esp_system.h"

#include "esp_camera.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
// #include "tensorflow/lite/system_setup.h"

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

static uint8_t s_led_state = 0;
bool usingModel = true;

float probabilityToTraverseAnyways = 0.2;
int runNumber = 0;
const int maxRuns = 100;

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
    bool firstcall = true;
    while (true)
    {
        bool goingForward = false;

        modelCall();

        //checking if frame is intraversible
        if (getLastClass1Prob() < CONFIDENCE_THRESHOLD)
        {
			float rngGoAnyways = (float)esp_random() / UINT32_MAX;
            if(rngGoAnyways < probabilityToTraverseAnyways)
            {
                moveForward();
                goingForward = true;
            }
            else
            {
                turnRight();
            }
            vTaskDelay(pdMS_TO_TICKS(200));
			ESP_LOGI("CONTROL", "traversable: %.2f", getLastClass1Prob());
        }
        //checking if frame is traversible (equal to or above the confidence threshold)
        else if (getLastClass1Prob() >= CONFIDENCE_THRESHOLD)
        {
            ESP_LOGI("CONTROL", "path is clear, driving forward");
			moveForward();
            goingForward = true;
			vTaskDelay(pdMS_TO_TICKS(200));
			ESP_LOGI("CONTROL", "path is clear, driving forward");
        }

        vTaskDelay(pdMS_TO_TICKS(100));

		// get label from last time
        
        // Update the accuracy

        int label = getLabel();
        resetAccumulation();
        stopMotors();
        // accuracy can only be emasured when we 'go forwards'
        if (goingForward)
        {
            
            if(((getLastClass1Prob() > CONFIDENCE_THRESHOLD) &&(label == 1)) || ((getLastClass1Prob() <= CONFIDENCE_THRESHOLD) && (label == 0)))
            {
                ESP_LOGI("acc", "CORRECT!");
                correctIncorrectArr.push_back(true);
            }
            else
            {
                ESP_LOGI("acc", "INCORRECT!");
                correctIncorrectArr.push_back(false);
            }
            ESP_LOGI("CONTROL", "Model was called!");
            // ESP_LOGI("CONTROL", "Accuracy: %f", ((double)correct)/runNumber);

            // writeToFile("Accuracy: " + std::to_string((double)correct / runNumber) + ", total inf latency: ");
            ESP_LOGI("CONTROL", "Continous learning was called label was %s", std::to_string(label).c_str());
            ESP_LOGI("CONTROL", "Last Class 1 prob %f", getLastClass1Prob());
            if(isHeadless) modelLearn(label); // i moved it from app_main so it runs in the same task as inference
        }

        runNumber++;
        if(runNumber == maxRuns)
        {
            break;
        }
    }

    std::string finalString = "Runs Completed!\n\nAccuracy Vector:\n";
    for(int i = 0; i < correctIncorrectArr.size(); i++)
    {
        finalString += std::to_string(correctIncorrectArr[i]) + ",";
    }
    // remove trailing comma
    finalString.pop_back();

    finalString += "\nInference Latency Vector (microseconds):\n";

    for(int i = 0; i < inferenceTimes.size(); i++)
    {
        finalString += std::to_string(inferenceTimes[i]) + ",";
    }

    finalString.pop_back();

    if(isHeadless)
    {
        finalString += "\nLearning Backprop Latency Vector (microseconds):\n";

        for(int i = 0; i < learnTimes.size(); i++)
        {
            finalString += std::to_string(learnTimes[i]) + ",";
        }
        finalString.pop_back();
    }

    writeToFile(finalString);

    signalDestroyedTask();
    vTaskDelete(NULL); 
}

extern "C" void app_main(void)
{
	littleFSInit();
    std::string contents = readFromFile(0); // Reads "/littlefs/0.txt"
    ESP_LOGI("FS", "File contents: %s", contents.c_str());
    vTaskDelay(pdMS_TO_TICKS(5000));

    sensorSetup();
    cameraInit();
    if (usingModel)
    {
        connectModel(isHeadless);
        setupModel();
    }
    ledc_setup();
    // doBlink();

    if (!isBmiReady || gotError || modelSetupFailed)
    {
        return;
    }


    xTaskCreatePinnedToCore(control_task, "control_task", 8192, NULL, 5, NULL, 0); // modellearn() is in here now btw
    xTaskCreatePinnedToCore(imu_task, "imu_task", 8192, NULL, 5, NULL, 1); // modellearn() is in here now btw
}