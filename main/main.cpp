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

#define PORT					30000
#define KEEPALIVE_IDLE			CONFIG_KEEPALIVE_IDLE
#define KEEPALIVE_INTERVAL		CONFIG_KEEPALIVE_INTERVAL
#define KEEPALIVE_COUNT			CONFIG_KEEPALIVE_COUNT

#define BLINK_GPIO GPIO_NUM_48
#define BLINK_PERIOD 1000

#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_DUTY_RES           LEDC_TIMER_13_BIT // Set duty resolution to 13 bits
#define LEDC_TIMER				LEDC_TIMER_0
#define LEDC_FREQUENCY  50 // Frequency in Hertz. Set frequency at 4 kHz

//YELLOW IS LEFT MOTOR, WHITE IS RIGHT MOTOR
#define LEFT_FRONT_MOTOR_PIN    GPIO_NUM_37 // Define the output GPIO
#define LEFT_BACK_MOTOR_PIN     GPIO_NUM_35 // Define the output GPIO
#define RIGHT_FRONT_MOTOR_PIN	GPIO_NUM_38 // Define the output GPIO
#define RIGHT_BACK_MOTOR_PIN	GPIO_NUM_36 // Define the output GPIO


#define LEDC_CHANNEL_LEFT_FRONT          LEDC_CHANNEL_0
#define LEDC_CHANNEL_LEFT_BACK            LEDC_CHANNEL_1
#define LEDC_CHANNEL_RIGHT_FRONT            LEDC_CHANNEL_2
#define LEDC_CHANNEL_RIGHT_BACK            LEDC_CHANNEL_3


#define FADE_RESOLUTION			10

#include "driver/i2c.h"

const int8_t PRINT_INTERVAL = 60;
int8_t framesUntilPrint = 60;
unsigned long previousTime = 0; // For loop timing


#define I2C_MASTER_SCL_IO           41      // Check your board's pins
#define I2C_MASTER_SDA_IO           42      
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          100000  // BMI270 supports Fast Mode
#define BMI270_ADDR                 0x68    // ADR pin to GND

void i2c_bus_init() {
    // 1. PHYSICAL RESET: Force pins to a known state first
    gpio_reset_pin((gpio_num_t)I2C_MASTER_SDA_IO);
    gpio_reset_pin((gpio_num_t)I2C_MASTER_SCL_IO);
    gpio_set_direction((gpio_num_t)I2C_MASTER_SDA_IO, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)I2C_MASTER_SCL_IO, GPIO_MODE_OUTPUT);
    
    // Hold them HIGH (idle state for I2C)
    gpio_set_level((gpio_num_t)I2C_MASTER_SDA_IO, 1);
    gpio_set_level((gpio_num_t)I2C_MASTER_SCL_IO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    // 2. NOW initialize the official driver
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = (gpio_num_t)I2C_MASTER_SDA_IO,
        .scl_io_num = (gpio_num_t)I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master = { .clk_speed = 100000 },
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_NUM, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0));
}

static timer_config_t config = {
    .alarm_en = TIMER_ALARM_DIS,        // don’t need alarm
    .counter_en = TIMER_PAUSE,         // Start the timer
    .intr_type = TIMER_INTR_LEVEL,     // Interrupt type
    .counter_dir = TIMER_COUNT_UP,     // Count upwards
    .auto_reload = TIMER_AUTORELOAD_DIS,               // Auto-reload timer. Don’t want this
    .clk_src = TIMER_SRC_CLK_APB,      // Clock source (APB)
	//currently, we are going to update the timer every half a millisecond, so we'll need to account for this.
    .divider = 40000,                     // Timer clock divider (40000 gives a 0.5 millisecond resolution. )
};

typedef struct sockaddr SA;
/* static const int ip_protocol = 0; */

static uint8_t s_led_state = 0;

static bool charging;
static bool inGame;
static bool resetting;

// used to decide when we start reversing or moving forward and such
static uint8_t lowerReverseBound = 5;
static uint8_t upperReverseBound = 7;
static uint8_t lowerForwardBound = 8;
static uint8_t upperForwardBound = 10;

TaskHandle_t doMovementHandle = NULL;
bool finishedMoving = false;
bool interruptMovement = false;

bool bmiReady = false;


//the way we do it is first index - left motor, second index - right motor.

//for current direction, ti's between -100 and 100. negative numbers mean reverse motor,
//and -100 reverses and greater speed than say -10, and 100 goes at faster spee than say
//10. We will later map this to the proper duty values
static float currentDirection[2] = {0, 0};
//stores the direction values we are currently at and our starting values
static int8_t currentTargets[2] = {0, 0};
static float startTargets[2] = {0, 0};

SemaphoreHandle_t waitForData;

espp::Bmi270<espp::bmi270::Interface::I2C>::Config bmi_config = {
    .device_address = BMI270_ADDR,
	.write = [](uint8_t dev_addr, const uint8_t *data, size_t len) {
		esp_err_t err = i2c_master_write_to_device(I2C_MASTER_NUM, dev_addr, data, len, 1000 / portTICK_PERIOD_MS);
		// Add a microscopic pause to let the BMI270 internal ASIC process the buffer
		ets_delay_us(100); 
		return err == ESP_OK;
	},
    .read = [](uint8_t dev_addr, uint8_t *data, size_t len) {
        esp_err_t err = i2c_master_read_from_device(I2C_MASTER_NUM, dev_addr, data, len, 1000 / portTICK_PERIOD_MS);
        return err == ESP_OK;
    }
};


std::unique_ptr<espp::Bmi270<espp::bmi270::Interface::I2C>> bmi;

typedef struct {
    int8_t fullForward[2];
    int8_t fullBack[2];
    int8_t forwardLeft[2];
    int8_t forwardRight[2];
	int8_t backLeft[2];
	int8_t backRight[2];
    int8_t fullLeft[2];
    int8_t fullRight[2];
    int8_t stop[2];
} MoveTargets;

// Initialize the struct after declaration. Basically depending on the input from the player, we will
// drive the esp to one of the following targets.
static MoveTargets moveTargets = {
    {85, 85},
    {-85, -85},
    {35, 100},
    {100, 35},
	{-35, -100},
	{-100, -35},
    {-50, 50},
    {50, -50},
    {0, 0}
};

//Raw Duty = (Percent/100) * (2^LEDC_DUTY_RES)

//Gets the raw duty value from the percentage from 0 to 100. 
float getRawDutyFromPercent(float duty){	
	//divide to convert from percent to decimal
	duty /= 100;	
	return (pow(2, (int) LEDC_DUTY_RES) * duty);
}

//from the direction going from -100 to 100, gets actual duty value needed to send to the motors
uint32_t getRawDutyFromBaseDirection(float direction)
{
    // direction: -100 to 100

    float pulse;

    if (direction > 0)
    {
        pulse = 1500 + (direction / 100.0) * 500; // 1500 → 2000
    }
    else if (direction < 0)
    {
        pulse = 1500 + (direction / 100.0) * 500; // 1500 → 1000
    }
    else
    {
        pulse = 1500;
    }

    return (pulse / 20000.0) * (1 << LEDC_DUTY_RES);
}

//converts pulse width (in ms) to the proper duty cycle RAW. Not sure if this is right, may have to check
float convertPulseWidthToPercentDuty(int pulseWidth)
{
	//get it with pulse width over period. Convert from microseconds to seconds too.
	return pulseWidth/(pow(10, 6)/LEDC_FREQUENCY) * 100;
}

void actuallyUpdateDuties(ledc_channel_t channel, float chosenDirection)
{
	ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, channel, chosenDirection));
	ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, channel));
}

void move(bool startup){
	if(startup)
	{
		//Move the left motor
		actuallyUpdateDuties(LEDC_CHANNEL_LEFT_FRONT, getRawDutyFromBaseDirection(currentDirection[0]));
		actuallyUpdateDuties(LEDC_CHANNEL_LEFT_BACK, getRawDutyFromBaseDirection(currentDirection[0]));
		actuallyUpdateDuties(LEDC_CHANNEL_RIGHT_FRONT, getRawDutyFromBaseDirection(-currentDirection[1]));
		actuallyUpdateDuties(LEDC_CHANNEL_RIGHT_BACK, getRawDutyFromBaseDirection(-currentDirection[1]));
	}
	else
	{
		actuallyUpdateDuties(LEDC_CHANNEL_LEFT_FRONT, getRawDutyFromBaseDirection(currentDirection[0]));
		actuallyUpdateDuties(LEDC_CHANNEL_LEFT_BACK, getRawDutyFromBaseDirection(currentDirection[0]));
		actuallyUpdateDuties(LEDC_CHANNEL_RIGHT_FRONT, getRawDutyFromBaseDirection(-currentDirection[1]));
		actuallyUpdateDuties(LEDC_CHANNEL_RIGHT_BACK, getRawDutyFromBaseDirection(-currentDirection[1]));
	}


    //ACTUAL DUTIES ARE: 56-89 FOR FORWARD (INCLUSIVE)
	//AND THEN 16 TO 49 FOR REVERSE (INCLUSIVE), 16 IS FASTER THAN 49
}

// sets up the PWM pins and timer
static void ledc_setup(){
	// Timer Configuration
	gpio_reset_pin(LEFT_FRONT_MOTOR_PIN);
	gpio_set_direction(LEFT_FRONT_MOTOR_PIN, GPIO_MODE_OUTPUT);


	gpio_reset_pin(LEFT_BACK_MOTOR_PIN);
	gpio_set_direction(LEFT_BACK_MOTOR_PIN, GPIO_MODE_OUTPUT);

	gpio_reset_pin(RIGHT_FRONT_MOTOR_PIN);
	gpio_set_direction(RIGHT_FRONT_MOTOR_PIN, GPIO_MODE_OUTPUT);

	gpio_reset_pin(RIGHT_BACK_MOTOR_PIN);
	gpio_set_direction(RIGHT_BACK_MOTOR_PIN, GPIO_MODE_OUTPUT);

	
	ledc_timer_config_t timer_conf = {
		.speed_mode = LEDC_MODE,
		.duty_resolution = LEDC_DUTY_RES,
		.timer_num = LEDC_TIMER,
		.freq_hz = LEDC_FREQUENCY,
		.clk_cfg = LEDC_AUTO_CLK
	};
	ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));
	// Channel Configuration
	ledc_channel_config_t leftFrontChannel = {
		.gpio_num = LEFT_FRONT_MOTOR_PIN,
        .speed_mode = LEDC_MODE,
		.channel = LEDC_CHANNEL_LEFT_FRONT,
		.intr_type = LEDC_INTR_DISABLE,
		.timer_sel = LEDC_TIMER,
		.duty = 0,
		.hpoint = 0
	};
	ESP_ERROR_CHECK(ledc_channel_config(&leftFrontChannel));

	//Channel Configuration
	ledc_channel_config_t leftBackChannel = {
		.gpio_num = LEFT_BACK_MOTOR_PIN,
        .speed_mode = LEDC_MODE,
		.channel = LEDC_CHANNEL_LEFT_BACK,
		.intr_type = LEDC_INTR_DISABLE,
		.timer_sel = LEDC_TIMER,
		.duty = 0,
		.hpoint = 0
	};
	ESP_ERROR_CHECK(ledc_channel_config(&leftBackChannel));

	//Channel Configuration
	ledc_channel_config_t rightFrontChannel = {
		.gpio_num = RIGHT_FRONT_MOTOR_PIN,
        .speed_mode = LEDC_MODE,
		.channel = LEDC_CHANNEL_RIGHT_FRONT,
		.intr_type = LEDC_INTR_DISABLE,
		.timer_sel = LEDC_TIMER,
		.duty = 0,
		.hpoint = 0
	};
	ESP_ERROR_CHECK(ledc_channel_config(&rightFrontChannel));

	//Channel Configuration
	ledc_channel_config_t rightBackChannel = {
		.gpio_num = RIGHT_BACK_MOTOR_PIN,
        .speed_mode = LEDC_MODE,
		.channel = LEDC_CHANNEL_RIGHT_BACK,
		.intr_type = LEDC_INTR_DISABLE,
		.timer_sel = LEDC_TIMER,
		.duty = 0,
		.hpoint = 0
	};
	ESP_ERROR_CHECK(ledc_channel_config(&rightBackChannel));

	//to make sure they're all full stopped
	currentDirection[0] = 0;
    currentDirection[1] = 0;

	//Move the motors to start position
    move(true);
}

//just quickly putting the on-chip LED to high
void doBlink()
{
	gpio_reset_pin(BLINK_GPIO);
	gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
	s_led_state = 1;
	gpio_set_level(BLINK_GPIO, s_led_state);
}

void i2c_bus_recovery() {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << I2C_MASTER_SCL_IO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io_conf);

    // Pulse SCL 9 times to clear any stuck state in the IMU
    for (int i = 0; i < 9; i++) {
        gpio_set_level((gpio_num_t)I2C_MASTER_SCL_IO, 0);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level((gpio_num_t)I2C_MASTER_SCL_IO, 1);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void sensor_task(void *pvParameters) {
    std::error_code ec;

    // Give the sensor a massive 3-second head start to boot its internal controller
    vTaskDelay(pdMS_TO_TICKS(3000)); 

	i2c_bus_recovery();
    
    i2c_bus_init();

    bmi = std::make_unique<espp::Bmi270<espp::bmi270::Interface::I2C>>(bmi_config);
    
    // Loop several times to give the sensor a chance to respond
    for (int i = 0; i < 5; i++) {
        if (bmi->init(ec)) {
            bmiReady = true;
            ESP_LOGI("BMI270", "SUCCESS! Sensor is alive on Battery.");
            break; 
        }
        ESP_LOGW("BMI270", "Init attempt %d failed: %s. Retrying...", i+1, ec.message().c_str());
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (!bmiReady) {
        ESP_LOGE("BMI270", "CRITICAL: Could not find sensor. Check battery voltage!");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        float dt = 1.0f; // Note: Use actual timing if doing balance math later!
		auto start = esp_timer_get_time();
        if (bmi->update(dt, ec)) {
            auto accel = bmi->get_accelerometer();
            auto gyro = bmi->get_gyroscope();
            printf("Accel: [%.2f, %.2f, %.2f] Gyro: [%.2f, %.2f, %.2f]\n",
                accel.x, accel.y, accel.z, gyro.x, gyro.y, gyro.z);

			auto elapsed = esp_timer_get_time() - start;
			fmt::print("Update time: {} µs\n", elapsed);
        }
        vTaskDelay(pdMS_TO_TICKS(1000)); // 1Hz update rate
    }
}

extern "C" void app_main(void) {

	vTaskDelay(pdMS_TO_TICKS(5000));
	
	xTaskCreate(sensor_task, "sensor_task", 10240, NULL, 5, NULL);

	while(!bmiReady) { vTaskDelay(pdMS_TO_TICKS(500)); }
    ledc_setup();
	doBlink();
	currentDirection[0] = 2;
	currentDirection[1] = -2;
	move(false);
	for(int i = 2; i < 100; i++)
	{
		currentDirection[0] = i;
		currentDirection[1] = -i;
		move(false);
		vTaskDelay(pdMS_TO_TICKS(50));
	}
    vTaskDelay(pdMS_TO_TICKS(10000));

    // Just launch the task and let it run
    
    // app_main can now just chill or handle other things (like WiFi/HTTP)
    while(1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}