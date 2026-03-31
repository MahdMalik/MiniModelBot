#include "camera.h"

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

bool gotError = false;

int BUFFER_SIZE = 10000;

// set up uart communication
uart_config_t uart_config = {
    .baud_rate = 115200,
    .data_bits = UART_DATA_8_BITS,
    .parity    = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
};

void cameraInit()
{
    // Configure UART parameters
    uart_param_config(UART_NUM_0, &uart_config);

    // gotta set up the pin apparently too
    uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // and install drivers like this? man esp idf is weird
    uart_driver_install(UART_NUM_0, BUFFER_SIZE, 0, 0, NULL, 0);

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

        .ledc_timer = LEDC_TIMER_1,
        .ledc_channel = LEDC_CHANNEL_4,

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
}

camera_fb_t* getCamFrame()
{
    camera_fb_t *theFrame = esp_camera_fb_get();
    if(theFrame == NULL)
    {
        CustomPrint("CAMERA", "Getting camera frame failed!");
    }
    else
    {
        
        CustomPrint("CAMERA", "Captured a camera frame with a length of %u bytes\n", theFrame->len);
    }
    return theFrame;
}
