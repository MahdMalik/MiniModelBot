#include "imu.h"

#define I2C_MASTER_SCL_IO           41      // Check your board's pins
#define I2C_MASTER_SDA_IO           42      
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          100000  // BMI270 supports Fast Mode
#define BMI270_ADDR                 0x68    // ADR pin to GND

bool bmiReady = false;

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
std::error_code ec;


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

void sensorSetup()
{
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
        return;
    }
}

void getSensorData()
{
    if(bmiReady)
    {
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
    }
}
