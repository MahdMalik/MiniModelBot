#include "model.h"
#include "headless_model.h"
#include "custom_layer.h"

#define modelWeights g_model
#define modelLen g_model_len

bool modelSetupFailed = false;
bool isHeadless = true;  // true = headless + custom head, false = original headed model

static CustomHead* customHead = nullptr;

// saying extern on these lets us know that we should find them in another file
extern const unsigned char modelWeights[];
extern const unsigned int modelLen;

uint8_t* tensorMemoryArea = nullptr; 
const int tensorMemorySize = 150 * 1024; // 300KB - plenty of room in PSRAM

const tflite::Model* model = nullptr;
static tflite::MicroMutableOpResolver<5> operationsManager;
alignas(tflite::MicroInterpreter) uint8_t buffer[sizeof(tflite::MicroInterpreter)];
static tflite::MicroInterpreter* interpreter = nullptr;

float theOutputScale;
int32_t theOutputZeroPoint;

const unsigned char* connectedModel = nullptr;
unsigned int connectedModelLen = 0;

void connectHeadlessModel(const unsigned char* modelData, unsigned int modelLength)
{
    connectedModel = modelData;
    connectedModelLen = modelLength;

    CustomPrint("MODEL", "Model connected! Size: %d bytes", connectedModelLen);
}

std::vector<float> extractFeatures()
{
    // Output tensor is [1, 12, 12, 16] — flatten to [2304]
    // TfLiteTensor* output = interpreter->tensor(interpreter->tensors_size() - 1);
    TfLiteTensor* output = interpreter->output(0);
    float   scale      = output->params.scale;
    int32_t zp         = output->params.zero_point;
    int     totalElems = 1 * 12 * 12 * 16;  // 2304

    std::vector<float> features(totalElems);
    for (int i = 0; i < totalElems; i++)
        features[i] = (output->data.int8[i] - zp) * scale;

    return features;
}

void setupModel()
{
    tensorMemoryArea = (uint8_t*)heap_caps_malloc(tensorMemorySize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    DenseLayer layer1 (32);
    DenseLayer layer2 (2);
    customHead = new CustomHead(layer1, layer2);
   if (tensorMemoryArea == nullptr) {
        CustomPrint("MODEL", "PSRAM Allocation failed! Is PSRAM enabled in menuconfig?");
        modelSetupFailed = true;
        return;
    }
    
    if (connectedModel == nullptr) {
        CustomPrint("MODEL", "No model connected!");
        modelSetupFailed = true;
        return;
    }

    model = tflite::GetModel(connectedModel);

    // have to check the model version matches what the library expecsts
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        CustomPrint("MODEL", "Model provided is schema version %d not equal to supported version %d.\n", model->version(), TFLITE_SCHEMA_VERSION);
        modelSetupFailed = true;
    }
    else
    {
        CustomPrint("MODEL", "Schema version matches");
    }

    if (operationsManager.AddFullyConnected() != kTfLiteOk
            || operationsManager.AddConv2D() != kTfLiteOk
            || operationsManager.AddMaxPool2D() != kTfLiteOk
            || operationsManager.AddMean() != kTfLiteOk
            || operationsManager.AddLogistic() != kTfLiteOk)
    {
        modelSetupFailed = true;
        CustomPrint("MODEL", "Couldn't add the CNN operations for some reason.");
    }
    else
    {
        CustomPrint("MODEL", "Added op scucessfully (?)");
    }

    interpreter = new (buffer) tflite::MicroInterpreter(
        model, operationsManager, tensorMemoryArea, tensorMemorySize
    );

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
        // Only grab output scale/zp for the headed model
        // headless model output goes through extractFeatures() instead
        if (!isHeadless)
        {
            // TODO: fill in correct tensor index once headed model is inspected
            // theOutputScale = interpreter->output(0)->params.scale;
            // theOutputZeroPoint = interpreter->output(0)->params.zero_point;
        }

        CustomPrint("MODEL", "thing worked out ok regarding the model!");
        
        // i want to see how much memory is left too
        size_t totalAvailable = heap_caps_get_total_size(MALLOC_CAP_8BIT);
        size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_8BIT);

        float percentUsed = (float) (totalAvailable - freeInternal) / totalAvailable * 100.0;

        CustomPrint("MEMORY", "Amount of used RAM is %.2f%%\n", percentUsed);

        CustomPrint("MODEL", "Running in %s mode", isHeadless ? "HEADLESS" : "ORIGINAL");
    }

    CustomPrint("MODEL", "The output scaling factor is %f\n", theOutputScale);
    CustomPrint("MODEL", "The output zero point number is actually %d\n", theOutputZeroPoint);
}

void modelCall()
{
    camera_fb_t *theFrame = getCamFrame();

    for (short i = 0; i < theFrame->len; i++)
    {
        interpreter->input(0)->data.int8[i] = (int8_t) theFrame->buf[i];
    }
    TfLiteStatus inferenceResult = interpreter->Invoke();

    if (inferenceResult != kTfLiteOk) 
    {
        CustomPrint("MODEL", "Invoke failed! What!?!?");
        modelSetupFailed = true;
    }
    else
    {
        CustomPrint("MODEL", "INVOCATION WORKED!!!! HALLELUJAH!!");
    }

    if (isHeadless)
    {
        // route through custom head for inference
        auto features = extractFeatures();
        auto probs    = customHead->forward(features);
        CustomPrint("MODEL", "Class 0: %.3f  Class 1: %.3f", probs[0], probs[1]);
    }
    else
    {
        // TODO: fill in correct output tensor index once headed model is inspected
        // int8_t stillQuantizedOutputClass0 = interpreter->output(0)->data.int8[0];

        //unquantize it this way, get class 1 prob from it easily then
        // float class0Prob = (float) (stillQuantizedOutputClass0 - theOutputZeroPoint) * theOutputScale;
        // float class1Prob = 1 - class0Prob;

        // CustomPrint("MODEL", "The probability of class 0 is is %f\n", class0Prob);
        // CustomPrint("MODEL", "The probability of class 1 is is %f\n", class1Prob);
    }

    // do this or else we'll use up all our memory in PSRAM
    esp_camera_fb_return(theFrame);
}

void modelLearn(int trueLabel)
{
    if (modelSetupFailed) return;

    if (!isHeadless) {
        CustomPrint("MODEL", "modelLearn() requires isHeadless=true — skipping");
        return;
    }

    camera_fb_t* theFrame = getCamFrame();

    for (short i = 0; i < theFrame->len; i++)
        interpreter->input(0)->data.int8[i] = (int8_t)theFrame->buf[i];

    TfLiteStatus inferenceResult = interpreter->Invoke();
    if (inferenceResult != kTfLiteOk) {
        CustomPrint("MODEL", "Invoke failed during learn!");
        // do this or else we'll use up all our memory in PSRAM
        esp_camera_fb_return(theFrame);
        return;
    }

    // do this or else we'll use up all our memory in PSRAM
    esp_camera_fb_return(theFrame);

    auto features = extractFeatures();
    customHead->train(features, trueLabel);

    static int trainCount = 0;
    if (++trainCount % 50 == 0)
        customHead->save();
}