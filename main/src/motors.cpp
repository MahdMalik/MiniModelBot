#include "motors.h"

#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_DUTY_RES           LEDC_TIMER_13_BIT // Set duty resolution to 13 bits
#define LEDC_TIMER				LEDC_TIMER_0
#define LEDC_FREQUENCY  50 // Frequency in Hertz. Set frequency at 4 kHz

//YELLOW IS LEFT MOTOR, WHITE IS RIGHT MOTOR
#define LEFT_FRONT_MOTOR_PIN    GPIO_NUM_1 // Define the output GPIO
#define LEFT_BACK_MOTOR_PIN     GPIO_NUM_2 // Define the output GPIO
#define RIGHT_FRONT_MOTOR_PIN	GPIO_NUM_40 // Define the output GPIO
#define RIGHT_BACK_MOTOR_PIN	GPIO_NUM_39 // Define the output GPIO


#define LEDC_CHANNEL_LEFT_FRONT          LEDC_CHANNEL_0
#define LEDC_CHANNEL_LEFT_BACK            LEDC_CHANNEL_1
#define LEDC_CHANNEL_RIGHT_FRONT            LEDC_CHANNEL_2
#define LEDC_CHANNEL_RIGHT_BACK            LEDC_CHANNEL_3

// used to decide when we start reversing or moving forward and such
uint8_t lowerReverseBound = 5;
uint8_t upperReverseBound = 7;
uint8_t lowerForwardBound = 8;
uint8_t upperForwardBound = 10;

//the way we do it is first index - left motor, second index - right motor.

//for current direction, ti's between -100 and 100. negative numbers mean reverse motor,
//and -100 reverses and greater speed than say -10, and 100 goes at faster spee than say
//10. We will later map this to the proper duty values
float currentDirection[2] = {0, 0};
//stores the direction values we are currently at and our starting values
int8_t currentTargets[2] = {0, 0};
float startTargets[2] = {0, 0};

timer_config_t config = {
    .alarm_en = TIMER_ALARM_DIS,        // don’t need alarm
    .counter_en = TIMER_PAUSE,         // Start the timer
    .intr_type = TIMER_INTR_LEVEL,     // Interrupt type
    .counter_dir = TIMER_COUNT_UP,     // Count upwards
    .auto_reload = TIMER_AUTORELOAD_DIS,               // Auto-reload timer. Don’t want this
    .clk_src = TIMER_SRC_CLK_APB,      // Clock source (APB)
	//currently, we are going to update the timer every half a millisecond, so we'll need to account for this.
    .divider = 40000,                     // Timer clock divider (40000 gives a 0.5 millisecond resolution. )
};

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
MoveTargets moveTargets = {
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
void ledc_setup(){
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