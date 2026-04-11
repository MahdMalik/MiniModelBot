#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_idf_version.h"
#include "esp_flash.h"
#include "esp_chip_info.h"
#include "spi_flash_mmap.h"

#include "esp_littlefs.h"
#include <fstream>
#include <iostream>


//tag for logging
static const char *TAG = "esp_littlefs";

// global conf struct known at compile time 
constexpr esp_vfs_littlefs_conf_t conf = {
    .base_path = "/littlefs",
    .partition_label = "storage",
    .format_if_mount_failed = true,
    .dont_mount = false,
};

//initializes the file system
void littleFSInit(){
    std::cout<<("Initializing LittleFS");

    // Use settings defined above to initialize and mount LittleFS filesystem at /littlefs.
    // Note: esp_vfs_littlefs_register is an all-in-one convenience function.
    esp_err_t ret = esp_vfs_littlefs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find LittleFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        }
        return;
    }
}

//get partition information and if not 
void getLFSPartitionInfo(){
    size_t total = 0, used = 0;
    esp_err_t ret = esp_littlefs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get LittleFS partition information (%s)", esp_err_to_name(ret));
        esp_littlefs_format(conf.partition_label);
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
}

// Use POSIX and C standard library functions to work with files.
// First create a file
static long fileNumber = 0;

//takes data in to file + file number name
void writeToFile(const char* data){
    std::cout<<("Opening file");

    std::string pathName="/littlefs/"+std::to_string(fileNumber) +".txt";

    std::ofstream MyFile(pathName);

    MyFile << data;

    MyFile.close();
    std::cout<<"File written";
}

void goToNewFile(){
    ++fileNumber;
    std::cout<<"Current file name changed to "<< fileNumber<<" file";
}