#include "imu.h"
#include "driver/i2c_master.h"

#define I2C_MASTER_SCL_IO           41
#define I2C_MASTER_SDA_IO           42
#define I2C_MASTER_NUM              I2C_NUM_0 // Try Port 1 to avoid Cam Port 0 conflicts
#define I2C_MASTER_FREQ_HZ          100000
#define BMI270_ADDR                 0x68

bool bmiReady = false;
static i2c_master_bus_handle_t bus_handle;
static i2c_master_dev_handle_t dev_handle;
//using type alias to reduce error
using RobotIMU = espp::Bmi270<espp::bmi270::Interface::I2C>;

//  Use the logic from your "Working" code
RobotIMU::Config bmi_config = {
    .device_address = BMI270_ADDR,
    .write = [](uint8_t dev_addr, const uint8_t *data, size_t len) {
        // Do NOT manually add the register byte; espp already put it in 'data'
        esp_err_t err = i2c_master_transmit(dev_handle, data, len, pdMS_TO_TICKS(1000));
        ets_delay_us(100); 
        return err == ESP_OK;
    },
    .read = [](uint8_t dev_addr, uint8_t *data, size_t len) {
        // Use raw receive like your working version
        esp_err_t err = i2c_master_receive(dev_handle, data, len, pdMS_TO_TICKS(1000));
        return err == ESP_OK;
    }
};

std::unique_ptr<RobotIMU> bmi;
std::error_code ec;

esp_err_t i2c_bus_init() {
    // 1. PHYSICAL PRE-CHARGE (Force pins HIGH because they lack resistors)
    gpio_reset_pin((gpio_num_t)I2C_MASTER_SDA_IO);
    gpio_reset_pin((gpio_num_t)I2C_MASTER_SCL_IO);
    gpio_set_direction((gpio_num_t)I2C_MASTER_SDA_IO, GPIO_MODE_OUTPUT_OD);
    gpio_set_direction((gpio_num_t)I2C_MASTER_SCL_IO, GPIO_MODE_OUTPUT_OD);
    gpio_set_level((gpio_num_t)I2C_MASTER_SDA_IO, 1);
    gpio_set_level((gpio_num_t)I2C_MASTER_SCL_IO, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    // 2. BUS CONFIG
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = (gpio_num_t) I2C_MASTER_SDA_IO,
        .scl_io_num = (gpio_num_t) I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 }
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BMI270_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };

    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle));

    return i2c_master_probe(bus_handle, BMI270_ADDR, 100);
}


// KEEP THIS (fine as-is)
void i2c_bus_recovery() {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << I2C_MASTER_SCL_IO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io_conf);

    for (int i = 0; i < 9; i++) {
        gpio_set_level((gpio_num_t)I2C_MASTER_SCL_IO, 0);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level((gpio_num_t)I2C_MASTER_SCL_IO, 1);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void sensorSetup()
{
    vTaskDelay(pdMS_TO_TICKS(3000)); 

    i2c_bus_recovery();
    i2c_bus_init();

    bmi = std::make_unique<RobotIMU>(bmi_config);
    
    for (int i = 0; i < 5; i++) {
        if (bmi->init(ec)) {
            bmiReady = true;
            ESP_LOGI("BMI270", "SUCCESS! Sensor is alive.");
            break; 
        }
        ESP_LOGW("BMI270", "Init attempt %d failed: %s", i+1, ec.message().c_str());
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (!bmiReady) {
        ESP_LOGE("BMI270", "CRITICAL: Could not find sensor!");
    }
}

IMUData getSensorData()
{
    if (bmiReady)
    {
        float dt = 1.0f;
        auto start = esp_timer_get_time();

        if (bmi->update(dt, ec)) {
            auto accel = bmi->get_accelerometer();
            auto gyro = bmi->get_gyroscope();

            printf("Accel: [%.2f, %.2f, %.2f] Gyro: [%.2f, %.2f, %.2f]\n",
                accel.x, accel.y, accel.z,
                gyro.x, gyro.y, gyro.z);

            auto elapsed = esp_timer_get_time() - start;
            printf("Update time: %lld us\n", elapsed);

            return {
                accel.x, accel.y, accel.z,
                gyro.x, gyro.y, gyro.z
            };
        }
    }
    return {};
}

// ... rest of your sensorSetup() and getSensorData() functions ...