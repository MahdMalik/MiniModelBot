#pragma once

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

#include "esp_camera.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
//#include "tensorflow/lite/system_setup.h"
#include "model_data.h"

#define PORT					30000
#define KEEPALIVE_IDLE			CONFIG_KEEPALIVE_IDLE
#define KEEPALIVE_INTERVAL		CONFIG_KEEPALIVE_INTERVAL
#define KEEPALIVE_COUNT			CONFIG_KEEPALIVE_COUNT

#define BLINK_GPIO GPIO_NUM_48
#define BLINK_PERIOD 1000


#define FADE_RESOLUTION			10

#define CONFIDENCE_THRESHOLD 0.6f
#define TURN_HURST_MS 400
#define DRIVE_FORWARD_POWER 20
#define TURN_POWER 100

static uint8_t s_led_state = 0;
bool usingModel = true;


// static const unsigned char *const modelWeights =
//     _content_drive_MyDrive_ACMResearchDataset_model_model_cnn_int8_tflite;
static const int TENSOR_ARENA_SIZE = 190000;
static uint8_t tensorArena[TENSOR_ARENA_SIZE];
static tflite::MicroInterpreter *interpreter = nullptr;
static float inputScale = 0.0f;
static int32_t inputZeroPoint = 0;
static float outputScale = 0.0f;
static int32_t outputZeroPoint = 0;


//just quickly putting the on-chip LED to high
void doBlink()
{
	gpio_reset_pin(BLINK_GPIO);
	gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
	s_led_state = 1;
	gpio_set_level(BLINK_GPIO, s_led_state);
}

static bool model_init(void)
{
    const tflite::Model *model = tflite::GetModel(modelWeights);
    if (model->version() != TFLITE_SCHEMA_VERSION)
    {
        ESP_LOGE("MODEL", "Schema version mismatch: %d", (int)model->version());
        return false;
    }

    static tflite::MicroMutableOpResolver<5> resolver;
    if (resolver.AddConv2D() != kTfLiteOk) return false;
    if (resolver.AddMaxPool2D() != kTfLiteOk) return false;
    if (resolver.AddMean() != kTfLiteOk) return false;
    if (resolver.AddFullyConnected() != kTfLiteOk) return false;
    if (resolver.AddLogistic() != kTfLiteOk) return false;

    static tflite::MicroInterpreter local_interpreter(model, resolver, tensorArena, TENSOR_ARENA_SIZE);
    interpreter = &local_interpreter;

    if (interpreter->AllocateTensors() != kTfLiteOk)
    {
        ESP_LOGE("MODEL", "failed to allocate tensors");
        return false;
    }

    inputScale = interpreter->input(0)->params.scale;
    inputZeroPoint = interpreter->input(0)->params.zero_point;
    outputScale = interpreter->output(0)->params.scale;
    outputZeroPoint = interpreter->output(0)->params.zero_point;

    ESP_LOGI("MODEL", "Model initialized successfully");
    return true;
}

static float run_inference(camera_fb_t *frame)
{
    int8_t *inputBuf = interpreter->input(0)->data.int8;
    for (size_t i = 0; i < frame->len; i++)
    {
        float norm = frame->buf[i] / 255.0f;
        int16_t q = (int16_t)roundf(norm / inputScale) + inputZeroPoint;
        if (q > 127) q = 127;
        if (q < -128) q = -128;
        inputBuf[i] = (int8_t)q;
    }

    if (interpreter->Invoke() != kTfLiteOk)
    {
        ESP_LOGE("MODEL", "Inference failed");
        return -1.0f;
    }

    int8_t rawOut = interpreter->output(0)->data.int8[0];
    float prob = (float)(rawOut - outputZeroPoint) * outputScale;
    if (prob < 0.0f) prob = 0.0f;
    if (prob > 1.0f) prob = 1.0f;
    return prob;
}

static void setMotors(float left, float right)
{
    currentDirection[0] = left;
    currentDirection[1] = right;
    move(false);
}

static void control_task(void *pvParameters)
{


    int turnDir = 0;

    // ramp motor up to move forward
    for (int i =0; i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        currentDirection[0] = i;
        currentDirection[1] = i;
        ESP_LOGI("MAIN", "Setting power: %d", i); 

        move(false);
    }


    while (true)
    {
        camera_fb_t *frame = esp_camera_fb_get();
        if (frame == nullptr)
        {
            ESP_LOGE("CAMERA", "Camera capture failed");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        float traversableProb = run_inference(frame);
        esp_camera_fb_return(frame);

        if (traversableProb < 0.0f)
        {
            setMotors(0, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        ESP_LOGI("CONTROL", "traversable: %.2f", traversableProb);

        if (traversableProb >= CONFIDENCE_THRESHOLD)
        {
            if (turnDir != 0)
            {
                ESP_LOGI("CONTROL", "path is clear, driving forward");
                turnDir = 0;
            }
            setMotors(DRIVE_FORWARD_POWER, DRIVE_FORWARD_POWER);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        else
        {
            setMotors(0, 0);
            vTaskDelay(pdMS_TO_TICKS(50));

            if (turnDir == 0)
            {
                turnDir = (esp_random() & 1) ? 1 : 2;
                ESP_LOGI("CONTROL", "obstacle, turning %s", turnDir == 1 ? "left" : "right");
            }

            if (turnDir == 1)
                setMotors(-TURN_POWER, TURN_POWER);
            else
                setMotors(TURN_POWER, -TURN_POWER);

            vTaskDelay(pdMS_TO_TICKS(TURN_HURST_MS));
            setMotors(0, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// testing with camera
extern "C" void main(void)
{
    vTaskDelay(pdMS_TO_TICKS(5000));

    sensorSetup();
    cameraInit();
    if (usingModel)
    {
        setupModel();
    }
    ledc_setup();
    // doBlink();

    // if (!isBmiReady || gotError || modelSetupFailed)
    // {
    //     return;
    // }

    //IMUData newData = getSensorData();

    ESP_LOGI("INFO", "it worked out!");

    if (!model_init())
    {
        ESP_LOGE("MAIN", "model initialization failed");
        while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    xTaskCreate(control_task, "control_task", 8192, NULL, 5, NULL);


    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); 
    }
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
