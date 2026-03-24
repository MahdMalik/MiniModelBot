#include "PrintFunctions.h"
#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "driver/uart.h"
#include "esp_log.h"
#include <cstring>

bool sendingImage;
bool inferenceWithoutSending;
uint8_t width = 0;
uint8_t height = 0;

void SetSendingImage(bool setter)
{
    sendingImage = setter;
}

void SetCanInference(bool setter)
{
  inferenceWithoutSending = setter;
}

void CustomPrint(const char* logType, const char* stringData, ...)
{
  if(!sendingImage)
  {
    // check if there's no percentage sign, so no formatting needed
    if(strchr(stringData, '%') == nullptr)
    {
      ESP_LOGI(logType, "%s", stringData);
    }
    else
    {
      //va list and args are a gift to god
      va_list extraItems;
      va_start(extraItems, stringData);

      esp_log_writev(ESP_LOG_INFO, logType, stringData, extraItems);

      va_end(extraItems);
    }
  }
}

bool ReadForReadiness()
{
  if(!sendingImage && inferenceWithoutSending)
  {
    return true;
  }
  
  size_t bufferLength = 0;
  uart_get_buffered_data_len(UART_NUM_0, &bufferLength);
  
  if(bufferLength == 1 && sendingImage)
  {
    uint8_t susVar;
    // check if we're able to read correctly
    int readResult = uart_read_bytes(UART_NUM_0, &susVar, 1, 100 / portTICK_PERIOD_MS);
    return readResult == 1;
  }
  return false;
}

void CustomWrite(size_t number)
{
  if(sendingImage)
  {
    uint8_t convertedToOneByteNum = (uint8_t) number;
    uart_write_bytes(UART_NUM_0, (const char *)&convertedToOneByteNum, 1);

    // Serial.write((uint8_t) number);
    if(width == 0)
    {
      width = number;
    }
    else if (height == 0)
    {
      height = number;
    }
  }
}

void CustomWrite(uint8_t* buf)
{
  if(sendingImage)
  {
    uart_write_bytes(UART_NUM_0, (const char*) buf, width * height);
  }
}

void CustomWrite(float number)
{
  if(sendingImage)
  {
    uart_write_bytes(UART_NUM_0, (const char *)&number, 4);
  }
}