#include "model.h"

bool modelSetupFailed = false;

// saying extern on these lets us know that we should find them in another file
extern const unsigned char modelWeights[];
extern const unsigned int modelLen;

const int tensorMemorySize = 190000;
uint8_t tensorMemoryArea[tensorMemorySize];

const tflite::Model* model;
static tflite::MicroMutableOpResolver<5> operationsManager;
static tflite::MicroInterpreter interpreter (model, operationsManager, tensorMemoryArea, tensorMemorySize);

float theOutputScale;
int32_t theOutputZeroPoint;

void setupModel()
{
    model = tflite::GetModel(modelWeights);

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

    // Allocate memory from the tensor_arena for the model's tensors.
    TfLiteStatus checkAllocationSuccess = interpreter.AllocateTensors();
    if (checkAllocationSuccess != kTfLiteOk) 
    {
        CustomPrint("MODEL", "AllocateTensors() failed");
        modelSetupFailed = true;
    }
    else
    {
        CustomPrint("MODEL", "Allocate tensors success");
    }
    
    if(!modelSetupFailed)
    {
        CustomPrint("MODEL", "thing worked out ok regarding the model!");
        
        // i want to see how much memory is left too
        size_t totalAvailable = heap_caps_get_total_size(MALLOC_CAP_8BIT);
        size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_8BIT);

        float percentUsed = (float) (totalAvailable - freeInternal) / totalAvailable * 100.0;

        CustomPrint("MEMORY", "Amount of used RAM is %.2f%%\n", percentUsed);
    }

    theOutputScale = interpreter.output(0)->params.scale;
    theOutputZeroPoint = interpreter.output(0)->params.zero_point;

    CustomPrint("MODEL", "The output scaling factor is %f\n", theOutputScale);
    CustomPrint("MODEL", "The output zero point number is actually %d\n", theOutputZeroPoint);
}

void modelCall()
{
    camera_fb_t *theFrame = getCamFrame();

    for (short i = 0; i < theFrame->len; i++)
    {
        interpreter.input(0)->data.int8[i] = (int8_t) theFrame->buf[i];
    }
    TfLiteStatus inferenceResult = interpreter.Invoke();

    if (inferenceResult != kTfLiteOk) 
    {
        CustomPrint("MODEL", "Invoke failed! What!?!?");
        modelSetupFailed = true;
    }
    else
    {
        CustomPrint("MODEL", "INVOCATION WORKED!!!! HALLELUJAH!!");
    }
    
    int8_t stillQuantizedOutputClass0 = interpreter.output(0)->data.int8[0];

    //unquantize it this way, get class 1 prob from it easily then
    float class0Prob = (float) (stillQuantizedOutputClass0 - theOutputZeroPoint) * theOutputScale;
    float class1Prob = 1 - class0Prob;

    CustomPrint("MODEL", "The probability of class 0 is is %f\n", class0Prob);
    CustomPrint("MODEL", "The probability of class 1 is is %f\n", class1Prob);

    // do this or else we'll use up all our memory in PSRAM
    esp_camera_fb_return(theFrame);
}