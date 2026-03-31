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
#include "tensorflow/lite/system_setup.h"
#include "model_data.h"

#define PORT 30000
#define KEEPALIVE_IDLE CONFIG_KEEPALIVE_IDLE
#define KEEPALIVE_INTERVAL CONFIG_KEEPALIVE_INTERVAL
#define KEEPALIVE_COUNT CONFIG_KEEPALIVE_COUNT

#define BLINK_GPIO GPIO_NUM_48
#define BLINK_PERIOD 1000

#define FADE_RESOLUTION 10

static uint8_t s_led_state = 0;
bool usingModel = true;

// just quickly putting the on-chip LED to high
void doBlink()
{
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
    s_led_state = 1;
    gpio_set_level(BLINK_GPIO, s_led_state);
}

// void i2c_bus_recovery() {
//     gpio_config_t io_conf = {
//         .pin_bit_mask = (1ULL << I2C_MASTER_SCL_IO),
//         .mode = GPIO_MODE_OUTPUT,
//         .pull_up_en = GPIO_PULLUP_ENABLE,
//     };
//     gpio_config(&io_conf);

//     // Pulse SCL 9 times to clear any stuck state in the IMU
//     for (int i = 0; i < 9; i++) {
//         gpio_set_level((gpio_num_t)I2C_MASTER_SCL_IO, 0);
//         vTaskDelay(pdMS_TO_TICKS(5));
//         gpio_set_level((gpio_num_t)I2C_MASTER_SCL_IO, 1);
//         vTaskDelay(pdMS_TO_TICKS(5));
//     }
// }

// void sensor_task(void *pvParameters) {
//     std::error_code ec;

//     // Give the sensor a massive 3-second head start to boot its internal controller
//     vTaskDelay(pdMS_TO_TICKS(3000));

// 	i2c_bus_recovery();

//     i2c_bus_init();

//     bmi = std::make_unique<espp::Bmi270<espp::bmi270::Interface::I2C>>(bmi_config);

//     // Loop several times to give the sensor a chance to respond
//     for (int i = 0; i < 5; i++) {
//         if (bmi->init(ec)) {
//             bmiReady = true;
//             ESP_LOGI("BMI270", "SUCCESS! Sensor is alive on Battery.");
//             break;
//         }
//         ESP_LOGW("BMI270", "Init attempt %d failed: %s. Retrying...", i+1, ec.message().c_str());
//         vTaskDelay(pdMS_TO_TICKS(500));
//     }

//     if (!bmiReady) {
//         ESP_LOGE("BMI270", "CRITICAL: Could not find sensor. Check battery voltage!");
//         vTaskDelete(NULL);
//         return;
//     }

//     while (1) {
//         float dt = 1.0f; // Note: Use actual timing if doing balance math later!
// 		auto start = esp_timer_get_time();
//         if (bmi->update(dt, ec)) {
//             auto accel = bmi->get_accelerometer();
//             auto gyro = bmi->get_gyroscope();
//             printf("Accel: [%.2f, %.2f, %.2f] Gyro: [%.2f, %.2f, %.2f]\n",
//                 accel.x, accel.y, accel.z, gyro.x, gyro.y, gyro.z);

// 			auto elapsed = esp_timer_get_time() - start;
// 			fmt::print("Update time: {} µs\n", elapsed);
//         }
//         vTaskDelay(pdMS_TO_TICKS(1000)); // 1Hz update rate
//     }
// }

// Inference control loop

// alias the auto-gen array name from model_data.h to something more type-able
static const unsigned char *const modelWeights =
    _content_drive_MyDrive_ACMResearchDataset_model_model_cnn_int8_tflite;

// assign 190 KB for CNN (adjust as needed)
static const int TENSOR_ARENA_SIZE = 190000;
static uint8_t tensorArena[TENSOR_ARENA_SIZE];

static tflite::MicroInterpreter *interpreter = nullptr;
static float inputScale = 0.0f;
static int32_t inputZeroPoint = 0;
static float outputScale = 0.0f;
static int32_t outputZeroPoint = 0;

// confidence threshold for traversable
#define CONFIDENCE_THRESHOLD 0.6f
// how long to spin in bursts before re-checking the camera
#define TURN_HURST_MS 400
// motor power levels
#define DRIVE_FORWARD_POWER 75
#define TURN_POWER 60

// initialize the model
static bool model_init(void)
{
    const tflite::Model *model = tflite::GetModel(modelWeights);
    if (model->version() != TFLITE_SCHEMA_VERSION)
    {
        ESP_LOGE((int)model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }

    static tflite::MicroMutableOpResolver<5> resolver;
    if (resolver.AddConv2D() != kTfLiteOk)
        return false;
    if (resolver.AddMaxPool2D() != kTfLiteOk)
        return false;
    if (resolver.AddMean() != kTfLiteOk)
        return false;
    if (resolver.AddFullyConnected() != kTfLiteOk)
        return false;
    if (resolver.AddLogistic() != kTfLiteOk)
        return false;

    static tflite : MicroInterpreter local_interpreter(model, resolver, tensorArena, TENSOR_ARENA_SIZE);
    interpreter = &local_interpreter;

    if (interpreter->AllocateTensors() != kTfLiteOk)
    {
        ESP_LOGE("failed to allocate tensors");
        return false;
    }

    inputScale = interpreter->input(0)->params.scale;
    inputZeroPoint = interpreter->input(0)->params.zero_point;
    outputScale = interpreter->output(0)->params.scale;
    outputZeroPoint = interpreter->output(0)->params.zero_point;

    ESP_LOGI("Model initialized successfully");
    return true;
}

// initialize the camera
static bool camera_init()
{
    // i generated these configs with AI so im not sure if they're right
    camera_config_t cfg = {
        .pin_pwdn = -1,
        .pin_reset = -1,
        .pin_xclk = 15,
        .pin_d7 = 16,
        .pin_d6 = 17,
        .pin_d5 = 18,
        .pin_d4 = 12,
        .pin_d3 = 10,
        .pin_d2 = 8,
        .pin_d1 = 9,
        .pin_d0 = 11,
        .pin_vsync = 6,
        .pin_href = 7,
        .pin_pclk = 13,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_1,
        .ledc_channel = LEDC_CHANNEL_4,
        .pixel_format = PIXFORMAT_GRAYSCALE,
        .frame_size = FRAMESIZE_96X96,
        .jpeg_quality = 10,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST};
    cfg.pin_soob_sda = 4;
    cfg.pin_soob_scl = 5;

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE("Camera init failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI("Camera initialized successfully");
    return true;
}

// run one inference on a frame then returns probability of traversable
static float run_inference(camera_fb_t *frame)
{
    int8_t *inputBuf = interpreter->input(0)->data.int8;
    for (size_t i = 0; i < frame->len; i++)
    {
        float norm = frame->buf[i] / 255.0f;
        int16t_t q = (int16_t)roundf(norm / inputScale) + inputZeroPoint;
        if (q > 127)
            q = 127;
        if (q < -128)
            q = -128;
        inputBuf[i] = (int8_t)q;
    }

    if (interpreter->Invoke() != kTfLiteOk)
    {
        ESP_LOGE("Inference failed");
        return -1.0f;
    }

    int8_t rawOut = interpreter->output(0)->data.int8[0];
    float prob = (float)(rawOut - outputZeroPoint) * outputScale;
    if (prob < 0.0f)
        prob = 0.0f;
    if (prob > 1.0f)
        prob = 1.0f;
    return prob;
}

static void setMotors(float left, float right)
{
    currentDirection[0] = left;
    currentDirection[1] = right;
    move(false);
}

// capture -> infer -> drive forward or turn until clear
static void control_task(void *pvParameters)
{
    // 0 = no direction chosen yet, 1 = turning left, 2 = turning right
    int turnDir = 0;
    while (true)
    {
        camera_fb_t *frame = esp_camera_fb_get();
        if (frame == nullptr)
        {
            ESP_LOGE("Camera capture failed");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        float traversableProb = run_inference(frame);
        esp_camera_fb_return(frame);

        if (traversableProb < 0.0f)
        { // prob an inference error so stop and wait
            setMotors(0, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue
        }

        ESP_LOGI("traversable: %.2f", traversableProb);

        if (traversableProb >= TRAVERASBLE_THRESHOLD)
        {
            if (turnDir != 0)
            {
                ESP_LOGI("path is clear, driving forward");
                turnDir = 0;
            }
            setMotors(DRIVE_FORWARD_POWER, DRIVE_FORWARD_POWER);
            vTaskDelay(pdMS_TO_TICKS(100)); // drive a little before next check
        }
        else
        {
            // blocked path, we need to pick a random turn direction and use it
            setMotors(0, 0); // stop before turning
            vTaskDelay(pdMS_TO_TICKS(50));

            if (turnDir == 0)
            {
                turnDir = (esp_random() & 1) ? 1 : 2; // random turn direction
                ESP_LOGI("obstacle, turning %s", turnDir == 1 ? "left" : "right");
            }

            // spin
            if (turnDir == 1)
            {
                setMotors(-TURN_POWER, TURN_POWER); // turn left
            }
            else
            {
                setMotors(TURN_POWER, -TURN_POWER); // turn right
            }
            vTaskDelay(pdMS_TO_TICKS(TURN_HURST_MS));
            setMotors(0, 0);                // stop after turn burst
            vTaskDelay(pdMS_TO_TICKS(100)); // brief pause before next frame
        }
    }
}

extern "C" void app_main(void)
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

    if (!bmiReady || gotError || modelSetupFailed)
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

    for (int i = 0; i < 100; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(100)); // 100ms * 100 = 10 seconds
    }

    ESP_LOGI("INFO", "it worked out!");

    // Just launch the task and let it run
    if (!model_init())
    {
        ESP_LOGE("model initialization failed");
        while (true)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    if (!camera_init())
    {
        ESP_LOGE("camera initialization failed");
        while (true)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    xTaskCreate(control_task, "control_task", 8192, NULL, 5, NULL);

    // app_main can now just chill or handle other things (like WiFi/HTTP)
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}