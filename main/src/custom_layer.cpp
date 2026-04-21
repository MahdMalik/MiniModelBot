#include "custom_layer.h"
#include <cmath>
#include <cstring>
#include <cstdlib>
#include "esp_log.h"
#include "model_data.h"
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/schema/schema_generated.h>
using namespace std;

static const char* TAG = "CUSTOM_LAYER";

bool keepingUpdatedModel = false;

float BCE_Loss(const vector<float>& probs, int label){
    float p;
    if (probs.size() == 1) {
        // Sigmoid binary case: probs[0] = P(class=1)
        // For label=1 use probs[0], for label=0 use 1 - probs[0]
        p = (label == 1) ? probs[0] : 1.0f - probs[0];
    } else {
        // Softmax multi-class case: index directly
        p = probs[label];
    }
    p = max(1e-7f, min(1.0f - 1e-7f, p));
    return -logf(p);
}

// Math helpers
static float sigmoid(float x){
    return 1.0f / (1.0f + exp(-x));
}
static float relu(float x){
    return x > 0.0f ? x : 0.0f;
}
// Xavier uniform initialization
static float xavierRand(int fanIn, int fanOut) {
    float limit = sqrtf(6.0f / (fanIn + fanOut));
    return ((float)rand() / RAND_MAX) * 2.0f * limit - limit;
}

// This method is the one that allocates all the space for the weight and bias vectors
// USED in other init() methods...
void DenseLayer::init(int inSize, int outSize){
    inputSize  = inSize;
    outputSize = outSize;

    weights.assign(outputSize * inputSize, 0.0f);
    biases.assign(outputSize, 0.0f);
    m_w.assign(outputSize * inputSize, 0.0f);
    v_w.assign(outputSize * inputSize, 0.0f);
    m_b.assign(outputSize, 0.0f);
    v_b.assign(outputSize, 0.0f);
}

// Initialize the layer with the activation values of the last layer from the .tflite file...
void DenseLayer::initFromLoadedModel(int inSize, int outSize) {
    // FIX: Call init() first to allocate Adam moment vectors (m_w, v_w, m_b, v_b)
    // Without this, backward() crashes on first training step
    init(inSize, outSize);

    const tflite::Model* loadedModel = tflite::GetModel(modelWeights);
    if (loadedModel->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Schema version mismatch!");
        initRandom(inSize, outSize);
        return;
    }

    const tflite::SubGraph* subgraph = loadedModel->subgraphs()->Get(0);
    if (subgraph == nullptr) {
        ESP_LOGE(TAG, "No subgraph found!");
        initRandom(inSize, outSize);
        return;
    }

    // Scan backwards for the last FullyConnected op specifically.
    // BuiltinOperator_FULLY_CONNECTED = 9
    const tflite::Operator* fcOp = nullptr;
    for (int i = (int)subgraph->operators()->size() - 1; i >= 0; i--) {
        const tflite::Operator* op = subgraph->operators()->Get(i);
        auto builtinCode = loadedModel->operator_codes()
                       ->Get(op->opcode_index())
                       ->builtin_code();
        if (builtinCode == tflite::BuiltinOperator_FULLY_CONNECTED) {
            fcOp = op;
            break;
        }
    }

    if (fcOp == nullptr) {
        ESP_LOGE(TAG, "No FullyConnected op found in model!");
        initRandom(inSize, outSize);
        return;
    }

    // FullyConnected inputs: [0]=input, [1]=weights, [2]=biases
    int weightTensorIndex = fcOp->inputs()->Get(1);
    int biasTensorIndex   = fcOp->inputs()->Get(2);

    // ── Extract weights ──
    const tflite::Tensor* weightTensor = subgraph->tensors()->Get(weightTensorIndex);
    const tflite::Buffer* weightBuffer = loadedModel->buffers()->Get(weightTensor->buffer());

    if (weightBuffer == nullptr || weightBuffer->data() == nullptr) {
        ESP_LOGE(TAG, "Weight buffer is empty!");
        initRandom(inSize, outSize);
        return;
    }

    float wScale     = 1.0f;
    int   wZeroPoint = 0;
    auto* wQuant = weightTensor->quantization();
    if (wQuant && wQuant->scale() && wQuant->scale()->size() > 0) {
        wScale     = wQuant->scale()->Get(0);
        wZeroPoint = (int)wQuant->zero_point()->Get(0);
    } else {
        ESP_LOGW(TAG, "Weight tensor has no quantization params — using scale=1, zp=0");
    }

    int numWeights = (int)weightBuffer->data()->size(); // size in bytes for int8
    if (numWeights != inSize * outSize) {
        ESP_LOGE(TAG, "Weight size mismatch! Got %d, expected %d", numWeights, inSize * outSize);
        initRandom(inSize, outSize);
        return;
    }

    const int8_t* rawWeights = reinterpret_cast<const int8_t*>(weightBuffer->data()->data());
    for (int i = 0; i < numWeights; i++)
        weights[i] = (rawWeights[i] - wZeroPoint) * wScale;

    // ── Extract biases ──
    const tflite::Tensor* biasTensor = subgraph->tensors()->Get(biasTensorIndex);
    const tflite::Buffer* biasBuffer = loadedModel->buffers()->Get(biasTensor->buffer());

    if (biasBuffer == nullptr || biasBuffer->data() == nullptr) {
        ESP_LOGE(TAG, "Bias buffer is empty!");
        biases.assign(outSize, 0.0f);
        return;
    }

    float bScale     = 1.0f;
    int   bZeroPoint = 0;
    auto* bQuant = biasTensor->quantization();
    if (bQuant && bQuant->scale() && bQuant->scale()->size() > 0) {
        bScale     = bQuant->scale()->Get(0);
        bZeroPoint = (int)bQuant->zero_point()->Get(0);
    } else {
        ESP_LOGW(TAG, "Bias tensor has no quantization params — using scale=1, zp=0");
    }

    int numBiases = (int)biasBuffer->data()->size() / sizeof(int32_t);
    if (numBiases != outSize) {
        ESP_LOGE(TAG, "Bias size mismatch! Got %d, expected %d", numBiases, outSize);
        biases.assign(outSize, 0.0f);
        return;
    }

    const int32_t* rawBiases = reinterpret_cast<const int32_t*>(biasBuffer->data()->data());
    for (int i = 0; i < numBiases; i++)
        biases[i] = (rawBiases[i] - bZeroPoint) * bScale;

    ESP_LOGI(TAG, "Loaded FC layer weights [%d] and biases [%d] from model", numWeights, numBiases);
}

// Initialize the layer with random values: use when not loading from flashed c-array model
void DenseLayer::initRandom(int inSize, int outSize){
    init(inSize, outSize);
    for(auto& w : weights){
        w = xavierRand(inputSize, outputSize);
        // biases kept at 0
    }
}

// Forward pass
// isLastLayer=true: apply sigmoid (1 output) or softmax (2+ outputs)
// isLastLayer=false: apply ReLU (hidden layer)
vector<float> DenseLayer::forward(const vector<float> &input, bool isLastLayer){
    vector<float> output(outputSize);
    for (int o = 0; o < outputSize; o++) {
        float sum = biases[o];
        for (int i = 0; i < inputSize; i++)
            sum += weights[o * inputSize + i] * input[i];
        output[o] = isLastLayer ? sum : relu(sum);  // raw logits for last layer
    }

    if (isLastLayer && outputSize == 1) {
        // Binary sigmoid: output[0] = P(class=1)
        output[0] = sigmoid(output[0]);
    } else if (isLastLayer) {
        // Softmax for multi-class
        float maxVal = *max_element(output.begin(), output.end());
        float sumExp = 0.0f;
        for (auto& v : output) { v = expf(v - maxVal); sumExp += v; }
        for (auto& v : output) v /= sumExp;
    }
    return output;
}

// isLastLayer=true: skip ReLU dead-neuron gate (sigmoid output is always in (0,1))
// isLastLayer=false: apply ReLU gate for hidden layers
void DenseLayer::backward(const vector<float>& input, const vector<float>& output, const vector<float>& gradOutput, bool isLastLayer) {
    timeStep++;
    float bc1 = 1.0f - powf(beta1, (float)timeStep);
    float bc2 = 1.0f - powf(beta2, (float)timeStep);

    for (int o = 0; o < outputSize; o++) {
        // FIX: Don't apply ReLU gate on last layer — sigmoid is always > 0
        // and the gradient is already computed correctly in CustomHead::backward()
        float actGrad = isLastLayer ? gradOutput[o]
                                    : (output[o] > 0.0f ? gradOutput[o] : 0.0f);

        m_b[o] = beta1 * m_b[o] + (1.0f - beta1) * actGrad;
        v_b[o] = beta2 * v_b[o] + (1.0f - beta2) * actGrad * actGrad;
        float m_b_hat = m_b[o] / bc1;
        float v_b_hat = v_b[o] / bc2;
        biases[o] -= learning_rate * m_b_hat / (sqrtf(v_b_hat) + epsilon);

        for (int i = 0; i < inputSize; i++) {
            float grad_w = actGrad * input[i];
            int   idx    = o * inputSize + i;

            m_w[idx] = beta1 * m_w[idx] + (1.0f - beta1) * grad_w;
            v_w[idx] = beta2 * v_w[idx] + (1.0f - beta2) * grad_w * grad_w;
            float m_w_hat = m_w[idx] / bc1;
            float v_w_hat = v_w[idx] / bc2;
            weights[idx] -= learning_rate * m_w_hat / (sqrtf(v_w_hat) + epsilon);
        }
    }
}

// NVS Persistence
bool DenseLayer::saveToNVS(const char* key) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("ml_weights", NVS_READWRITE, &handle);
    if (err != ESP_OK) return false;

    char wKey[32], bKey[32];
    snprintf(wKey, sizeof(wKey), "%s_w", key);
    snprintf(bKey, sizeof(bKey), "%s_b", key);

    nvs_set_blob(handle, wKey, weights.data(), weights.size() * sizeof(float));
    nvs_set_blob(handle, bKey, biases.data(),  biases.size()  * sizeof(float));
    nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "Saved layer '%s' (%d weights) to NVS", key, (int)weights.size());
    return true;
}

bool DenseLayer::loadFromNVS(const char* key) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("ml_weights", NVS_READONLY, &handle);
    if (err != ESP_OK) return false;

    char wKey[32], bKey[32];
    snprintf(wKey, sizeof(wKey), "%s_w", key);
    snprintf(bKey, sizeof(bKey), "%s_b", key);

    size_t wSize = weights.size() * sizeof(float);
    size_t bSize = biases.size()  * sizeof(float);

    bool ok = (nvs_get_blob(handle, wKey, weights.data(), &wSize) == ESP_OK)
           && (nvs_get_blob(handle, bKey, biases.data(),  &bSize) == ESP_OK);

    nvs_close(handle);
    if (ok) ESP_LOGI(TAG, "Loaded layer '%s' from NVS", key);
    else    ESP_LOGW(TAG, "No saved weights for '%s', using random init", key);
    return ok;
}

// CustomHead methods
void CustomHead::init(int featureSize, int outputSize){
    layer.initFromLoadedModel(featureSize, outputSize);

    if(keepingUpdatedModel) {
        if (!layer.loadFromNVS("custom")) {
            layer.initRandom(featureSize, outputSize);
            ESP_LOGI("LAYER", "HEY THIS DIDN'T WORK");
        }
    }
}

vector<float> CustomHead::forward(const vector<float>& features) {
    return layer.forward(features, true);
}

void CustomHead::backward(const vector<float>& features, const vector<float>& probs, int label) {
    float loss = BCE_Loss(probs, label);
    vector<float> gradLayer(layer.outputSize);

    if (layer.outputSize == 1) {
        // FIX: Sigmoid binary gradient
        // probs[0] = P(class=1), label is 0 or 1 (not an index into probs)
        gradLayer[0] = probs[0] - (float)label;
    } else {
        // Softmax gradient: dL/dlogit_o = p_o - 1(o==label)
        for (int o = 0; o < layer.outputSize; o++)
            gradLayer[o] = probs[o] - (o == label ? 1.0f : 0.0f);
    }

    // FIX: pass isLastLayer=true so backward() skips the ReLU dead-neuron gate
    layer.backward(features, probs, gradLayer, true);
}

void CustomHead::train(const std::vector<float>& features, int label) {
    auto probs = forward(features);
    backward(features, probs, label);
}

void CustomHead::save() {
    layer.saveToNVS("custom");
}

void CustomHead::load() {
    layer.loadFromNVS("custom");
}