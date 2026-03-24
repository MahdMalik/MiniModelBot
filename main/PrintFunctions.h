#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void SetSendingImage(bool setter);

void SetCanInference(bool setter);

void CustomPrint(const char* logType, const char* data, ...);

bool ReadForReadiness();

void CustomWrite(size_t number);

void CustomWrite(float number);

void CustomWrite(uint8_t* buf);