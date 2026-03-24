/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_camera.h"
#include "PrintFunctions.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"

#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

// the pinout doesn't have these
#define PWDN_PIN    -1
#define RESET_PIN   -1

// these are correct
#define SIOD_PIN    4
#define SIOC_PIN    5
#define VSYNC_PIN   6
#define HREF_PIN    7
#define XCLK_PIN    15
#define PCLK_PIN    13

// these pinouts are correct
#define Y7_PIN      16
#define Y6_PIN      17
#define Y5_PIN      18
#define Y4_PIN      12
#define Y3_PIN      10
#define Y2_PIN      8
#define Y1_PIN      9
#define Y0_PIN      11

// when in loop we have this so when there's an error, we can stop looping
bool gotError = false;

bool isSendingImage = true;

int BUFFER_SIZE = 10000;

bool canInferenceWithoutSending = false;

// saying extern on these lets us know that we should find them in another file
extern const unsigned char modelWeights[];
extern const unsigned int modelLen;

const int tensorMemorySize = 190000;
uint8_t tensorMemoryArea[tensorMemorySize];

extern "C" void app_main(void)
{
    // set up uart communication
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };

    // Configure UART parameters
    uart_param_config(UART_NUM_0, &uart_config);

    // gotta set up the pin apparently too
    uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // and install drivers like this? man esp idf is weird
    uart_driver_install(UART_NUM_0, BUFFER_SIZE, 0, 0, NULL, 0);

    CustomPrint("OTHER", "Battle start!");

    const tflite::Model* model = tflite::GetModel(modelWeights);

    // have to check the model version matches what the library expecsts
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        CustomPrint("MODEL", "Model provided is schema version %d not equal to supported version %d.\n", model->version(), TFLITE_SCHEMA_VERSION);
        gotError = true;
    }
    else
    {
        CustomPrint("MODEL", "Schema version matches");
    }


    // So resolver is basically what operations exist, so we want to say that we add the fully connected oeprations,
    // along with the conv 2d stuff, and see if that works fine
    static tflite::MicroMutableOpResolver<5> operationsManager;
    if (operationsManager.AddFullyConnected() != kTfLiteOk
            || operationsManager.AddConv2D() != kTfLiteOk
            || operationsManager.AddMaxPool2D() != kTfLiteOk
            || operationsManager.AddMean() != kTfLiteOk
            || operationsManager.AddLogistic() != kTfLiteOk)
    {
        gotError = true;
        CustomPrint("MODEL", "Couldn't add the CNN operations for some reason.");
    }
    else
    {
        CustomPrint("MODEL", "Added op scucessfully (?)");
    }

    // this is actually the thing that will execute the model and run through it
    static tflite::MicroInterpreter interpreter (model, operationsManager, tensorMemoryArea, tensorMemorySize);

    // Allocate memory from the tensor_arena for the model's tensors.
    TfLiteStatus checkAllocationSuccess = interpreter.AllocateTensors();
    if (checkAllocationSuccess != kTfLiteOk) 
    {
        CustomPrint("MODEL", "AllocateTensors() failed");
        gotError = true;
    }
    else
    {
        CustomPrint("MODEL", "Allocate tensors success");
    }


    if(!gotError)
    {
        CustomPrint("MODEL", "thing worked out ok regarding the model!");
        
        // i want to see how much memory is left too
        size_t totalAvailable = heap_caps_get_total_size(MALLOC_CAP_8BIT);
        size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_8BIT);

        float percentUsed = (float) (totalAvailable - freeInternal) / totalAvailable * 100.0;

        CustomPrint("MEMORY", "Amount of used RAM is %.2f%%\n", percentUsed);
    }
    
    
    SetSendingImage(isSendingImage);
    SetCanInference(canInferenceWithoutSending);


    // set our clock frequency to this for how often we read from the camera
    int CLOCK_FREQ = 20000000;


    // yippee i love structs and setting 10 morbillion parameters
    camera_config_t camConfig = {
        .pin_pwdn = -1,
        .pin_reset = -1,
        .pin_xclk = XCLK_PIN,

        .pin_d7 = Y7_PIN,
        .pin_d6 = Y6_PIN,
        .pin_d5 = Y5_PIN,
        .pin_d4 = Y4_PIN,
        .pin_d3 = Y3_PIN,
        .pin_d2 = Y2_PIN,
        .pin_d1 = Y1_PIN,
        .pin_d0 = Y0_PIN,

        .pin_vsync = VSYNC_PIN,
        .pin_href = HREF_PIN,
        .pin_pclk = PCLK_PIN,

        .xclk_freq_hz = CLOCK_FREQ,

        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_GRAYSCALE,
        .frame_size = FRAMESIZE_96X96,

        .jpeg_quality = 10,

        // must be set to this so we don't read old frames
        .fb_count = 2,

        .fb_location = CAMERA_FB_IN_PSRAM,

        .grab_mode = CAMERA_GRAB_LATEST
    };

    // can't set these in the initial initialization of the struct, so must do it here
    camConfig.pin_sccb_sda = SIOD_PIN;
    camConfig.pin_sccb_scl = SIOC_PIN;

    // initializes camera and returns if there's an error
    esp_err_t cameraError = esp_camera_init(&camConfig);

    // if error, display it, otherwise say we gucci
    if (cameraError != ESP_OK) 
    {
        CustomPrint("CAMERA", "Camera init failed with error 0x%x\n", cameraError);
        gotError = true;
    }
    else
    {
        CustomPrint("CAMERA", "Camera working!");
    } 



    // these are stuff to convert to and from int8 (which the model is stored as) to floats between 0 and 1
    float theInputScale = interpreter.input(0)->params.scale;
    int32_t theInputZeroPoint = interpreter.input(0)->params.zero_point;

    CustomPrint("MODEL", "The input scaling factor is %f\n", theInputScale);
    CustomPrint("MODEL", "The input zero point number is actually %d\n", theInputZeroPoint);

    float theOutputScale = interpreter.output(0)->params.scale;
    int32_t theOutputZeroPoint = interpreter.output(0)->params.zero_point;

    CustomPrint("MODEL", "The output scaling factor is %f\n", theOutputScale);
    CustomPrint("MODEL", "The output zero point number is actually %d\n", theOutputZeroPoint);


    while(1)
    {
        // if we didn't get error, run the code to fetch the camera
        if(!gotError)
        {
            camera_fb_t *theFrame = esp_camera_fb_get();

            if(theFrame == NULL)
            {
                CustomPrint("CAMERA", "Getting camera frame failed!");
            }
            else
            {
                
                CustomPrint("CAMERA", "Captured a camera frame with a length of %u bytes\n", theFrame->len);

                // for(uint8_t i = 0; i < theFrame->width; i++)
                // {
                //     CustomPrintf("%d ", theFrame->buf[i]);
                // }

                if(ReadForReadiness())
                {
                    // set up the input tensor to contain the camera data. Ensure that since the camera is in uint8_5, we have
                    // to convert that actually

                    for (short i = 0; i < theFrame->len; i++)
                    {
                        // normalize to between 0 and 1 first
                        float normalizedPixel = theFrame->buf[i] / 255.0;
                        // use input scale and zero point to convert it to a int8_t (stored as int16 for now in case of overload)
                        int16_t actualNumber = (int16_t)(round(normalizedPixel / theInputScale)) + theInputZeroPoint;
                        
                        // check if out of bounds, fix it if so
                        if (actualNumber > 127) actualNumber = 127;
                        if (actualNumber < -128) actualNumber = -128;


                        interpreter.input(0)->data.int8[i] = (int8_t) actualNumber;
                    }

                    // RUN INTERPRETER, PLEASE WORK
                    TfLiteStatus inferenceResult = interpreter.Invoke();
                    if (inferenceResult != kTfLiteOk) 
                    {
                        CustomPrint("MODEL", "Invoke failed! What!?!?");
                        gotError = true;
                    }
                    else
                    {
                        CustomPrint("MODEL", "INVOCATION WORKED!!!! HALLELUJAH!!");
                    }

                    // only need one number representing the first class's result
                    int8_t stillQuantizedOutputClass0 = interpreter.output(0)->data.int8[0];

                    //unquantize it this way, get class 1 prob from it easily then
                    float class0Prob = (float) (stillQuantizedOutputClass0 - theOutputZeroPoint) * theOutputScale;
                    float class1Prob = 1 - class0Prob;

                    CustomPrint("MODEL", "The probability of class 0 is is %f\n", class0Prob);
                    CustomPrint("MODEL", "The probability of class 1 is is %f\n", class1Prob);


                    // dl_model_run(&model, &tensorForCamInput, &modelResults);

                    // int8_t unknownLogit0 = modelResults.data[0];
                    // int8_t unknownLogit1 = modelResults.data[1];
                    
                    CustomWrite(theFrame->width);
                    CustomWrite(theFrame->height);
                    CustomWrite(theFrame->buf);
                    CustomWrite(class0Prob);
                    CustomWrite(class1Prob);
                }

                CustomPrint("SPACING", "-------------");

                // do this or else we'll use up all our memory in PSRAM
                esp_camera_fb_return(theFrame);
            }

        }
        // put your main code here, to run repeatedly:
        CustomPrint("OTHER", "HELP IN GAIA");

        // don't want to run the camera and print statement too many times, so run it every 2 secondss
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
