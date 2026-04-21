#include "model.h"
#include "headless_model.h"
#include "custom_layer.h"
#include "my_littlefs.h"
#include <new>
#include <iostream>

//counter for run numbers
int runNumber = 0;
int totalInfTime = 0;
int totalLearnTime = 0;
int correct = 0;
int incorrect = 0;


bool modelSetupFailed = false;
bool isHeadless = true; // true = headless + custom head, false = original headed model

static CustomHead *customHead = nullptr;

uint8_t *tensorMemoryArea = nullptr;
const int tensorMemorySize = 730 * 1024; // 300KB - plenty of room in PSRAM
static float lastClass1Prob = 0.0f;

const tflite::Model *model = nullptr;
static tflite::MicroMutableOpResolver<5> operationsManager;
static tflite::MicroInterpreter *interpreter = nullptr;

float theOutputScale = 0.0f;
int32_t theOutputZeroPoint = 0;

const unsigned char *connectedModel = nullptr;
unsigned int connectedModelLen = 0;

void setHeadlessMode(bool headless)
{
    isHeadless = headless;
}

void connectModel(const unsigned char *modelData, unsigned int modelLength, bool headlessMode)
{
    setHeadlessMode(headlessMode);
    connectedModel = modelData;
    connectedModelLen = modelLength;

    CustomPrint("MODEL", "Model connected! Size: %d bytes (%s mode)", connectedModelLen, isHeadless ? "HEADLESS" : "FULL");
}

std::vector<float> extractFeatures()
{
    // Output tensor is [1, 12, 12, 16] — flatten to [2304]
    // TfLiteTensor* output = interpreter->tensor(interpreter->tensors_size() - 1);
    TfLiteTensor *output = interpreter->output(0);
    float scale = output->params.scale;
    int32_t zp = output->params.zero_point;
    int totalElems = 1 * 12 * 12 * 16; // 2304

    std::vector<float> features(totalElems);
    for (int i = 0; i < totalElems; i++)
        features[i] = (output->data.int8[i] - zp) * scale;

    return features;
}

void setupModel()
{
    tensorMemoryArea = (uint8_t *)heap_caps_malloc(tensorMemorySize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (tensorMemoryArea == nullptr)
    {
        CustomPrint("MODEL", "PSRAM Allocation failed! Is PSRAM enabled in menuconfig?");
        modelSetupFailed = true;
        return;
    }

    if (isHeadless)
    {
        customHead = new CustomHead();
        // customHead->load();
        customHead->init(36, 1); // feature size, output size
    }

    if (connectedModel == nullptr)
    {
        CustomPrint("MODEL", "No model connected!");
        modelSetupFailed = true;
        return;
    }

    model = tflite::GetModel(connectedModel);

    // have to check the model version matches what the library expecsts
    if (model->version() != TFLITE_SCHEMA_VERSION)
    {
        CustomPrint("MODEL", "Model provided is schema version %d not equal to supported version %d.\n", model->version(), TFLITE_SCHEMA_VERSION);
        modelSetupFailed = true;
    }
    else
    {
        CustomPrint("MODEL", "Schema version matches");
    }

    if (operationsManager.AddFullyConnected() != kTfLiteOk || operationsManager.AddConv2D() != kTfLiteOk || operationsManager.AddMaxPool2D() != kTfLiteOk || operationsManager.AddMean() != kTfLiteOk || operationsManager.AddLogistic() != kTfLiteOk)
    {
        modelSetupFailed = true;
        CustomPrint("MODEL", "Couldn't add the CNN operations for some reason.");
    }
    else
    {
        CustomPrint("MODEL", "Added op scucessfully (?)");
    }

    interpreter = new tflite::MicroInterpreter(
        model, operationsManager, tensorMemoryArea, tensorMemorySize);

    // Allocate memory from the tensor_arena
    TfLiteStatus checkAllocationSuccess = interpreter->AllocateTensors();

    // Check how much memory is actually being used
    size_t used_bytes = interpreter->arena_used_bytes();

    if (checkAllocationSuccess != kTfLiteOk)
    {
        // If it fails, print the size you attempted to use vs. the arena size
        char buf[128];
        sprintf(buf, "AllocateTensors() failed. Used: %d bytes, Arena Size: %d",
                used_bytes, tensorMemorySize);
        CustomPrint("MODEL", buf);

        modelSetupFailed = true;
    }
    else
    {
        char buf[128];
        sprintf(buf, "Success! Used: %d bytes of %d", used_bytes, tensorMemorySize);
        CustomPrint("MODEL", buf);
    }

    if (!modelSetupFailed)
    {
        if (!isHeadless)
        {
            TfLiteTensor *output = interpreter->output(0);
            theOutputScale = output->params.scale;
            theOutputZeroPoint = output->params.zero_point;
        }

        CustomPrint("MODEL", "thing worked out ok regarding the model!");

        // i want to see how much memory is left too
        size_t totalAvailable = heap_caps_get_total_size(MALLOC_CAP_8BIT);
        size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_8BIT);

        float percentUsed = (float)(totalAvailable - freeInternal) / totalAvailable * 100.0;

        CustomPrint("MEMORY", "Amount of used RAM is %.2f%%\n", percentUsed);

        CustomPrint("MODEL", "Running in %s mode", isHeadless ? "HEADLESS" : "ORIGINAL");
    }

    CustomPrint("MODEL", "The output scaling factor is %f\n", theOutputScale);
    CustomPrint("MODEL", "The output zero point number is actually %d\n", theOutputZeroPoint);
}

//calls the model 
void modelCall()
{
    camera_fb_t *theFrame = getCamFrame();

    for (short i = 0; i < theFrame->len; i++)
    {
        interpreter->input(0)->data.int8[i] = (int8_t)theFrame->buf[i];
    }
    auto startInfTime = esp_timer_get_time();
    TfLiteStatus inferenceResult = interpreter->Invoke();
    // TODO: send this to file system on esp32

    totalInfTime += esp_timer_get_time() - startInfTime;

    // writeToFile("Total inference time (microseconds) for static model: "+ std::to_string(totalInfTime)+"\nRun number: " +std::to_string(runNumber));

    if (inferenceResult != kTfLiteOk)
    {
        CustomPrint("MODEL", "Invoke failed! What!?!?");
        modelSetupFailed = true;
    }
    else
    {
        CustomPrint("MODEL", "INVOCATION WORKED!!!! HALLELUJAH!!");
    }

    float class0Prob = 0;
    float class1Prob = 0;
    if (isHeadless)
    {
        // route through custom head for inference
        auto features = extractFeatures();
        auto probs = customHead->forward(features);

        class0Prob = probs[0];
        // class1Prob = probs[1];
        class1Prob = 1 - probs[0];

        ESP_LOGI("MODEL", "Loss: %.4f", BCE_Loss(probs, class0Prob > class1Prob ? 0 : 1));
    }
    else
    {
        TfLiteTensor *output = interpreter->output(0);
        int outputElements = 1;
        for (int i = 0; i < output->dims->size; ++i)
            outputElements *= output->dims->data[i];

        if (output->type == kTfLiteInt8)
        {
            if (outputElements >= 2)
            {
                class0Prob = (output->data.int8[0] - output->params.zero_point) * output->params.scale;
                class1Prob = (output->data.int8[1] - output->params.zero_point) * output->params.scale;
            }
            else if (outputElements == 1)
            {
                class0Prob = (output->data.int8[0] - output->params.zero_point) * output->params.scale;
                class1Prob = 1 - class0Prob;
            }
        }
        else if (output->type == kTfLiteFloat32)
        {
            if (outputElements >= 2)
            {
                class0Prob = output->data.f[0];
                class1Prob = output->data.f[1];
            }
            else if (outputElements == 1)
            {
                class0Prob = output->data.f[0];
                class1Prob = 1 - class0Prob;
            }
        }
        else
        {
            CustomPrint("MODEL", "Unsupported output tensor type %d", output->type);
        }
    }

    ESP_LOGI("MODEL", "Class 0: %.3f  Class 1: %.3f", class0Prob, class1Prob);

    lastClass1Prob = class1Prob;

    // do this or else we'll use up all our memory in PSRAM
    esp_camera_fb_return(theFrame);

    //increment after run works
    ++runNumber;
}

float getLastClass1Prob()
{
    
    return lastClass1Prob;
}

void modelLearn(int trueLabel)
{
    auto modelLearnStartTime=esp_timer_get_time();
    if (modelSetupFailed)
    {
        return;
    }
        

    if (!isHeadless)
    {
        CustomPrint("MODEL", "modelLearn() requires isHeadless=true — skipping");
        return;
    }

    auto features = extractFeatures();
    customHead->train(features, trueLabel);
    totalLearnTime+= esp_timer_get_time()-modelLearnStartTime;
    ESP_LOGI("MODEL", "total learn time was %d", totalLearnTime);

    static int trainCount = 0;
    if (++trainCount % 10 == 0 && keepingUpdatedModel)
        customHead->save();
    // writeToFile("Total inference time for static model and continuous: "+ std::to_string(totalLearnTime)+"\nRun number: " +std::to_string(runNumber));
    ESP_LOGI("CONTINUOUS LEARNING", "CONTINUOUS LEARNING SUCCESS!");
}