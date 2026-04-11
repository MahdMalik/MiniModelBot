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

float BCE_Loss(const vector<float>& probs, int label){
  // Clamp to avoid log(0)
  float p = max(1e-7f, min(1.0f - 1e-7f, probs[label]));
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
// Create the initialize methods...

// This method is the one that allocates all the space for the weight and bias vectors
// USED in other init() methods...
void DenseLayer::init(int inSize, int outSize){
  inputSize = inSize;
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
  inputSize  = inSize;
  outputSize = outSize;

  m_w.assign(outSize * inSize, 0.0f);
  v_w.assign(outSize * inSize, 0.0f);
  m_b.assign(outSize, 0.0f);
  v_b.assign(outSize, 0.0f);
  timeStep = 0;

  const tflite::Model* loadedModel = tflite::GetModel(modelWeights);

  if(loadedModel->version() != TFLITE_SCHEMA_VERSION) {
    ESP_LOGE(TAG, "Schema version mismatch!");
    initRandom(inSize, outSize);
    return;
  }

  const tflite::SubGraph* subgraph = loadedModel->subgraphs()->Get(0);
  if(subgraph == nullptr) {
    ESP_LOGE(TAG, "No subgraph found!");
    initRandom(inSize, outSize);
    return;
  }

  // Get the last operator in the subgraph — that's our final layer
  int lastOpIndex = subgraph->operators()->size() - 1;
  const tflite::Operator* lastOp = subgraph->operators()->Get(lastOpIndex);

  // For a FullyConnected layer: inputs[0]=input, inputs[1]=weights, inputs[2]=biases
  int weightTensorIndex = lastOp->inputs()->Get(1);
  int biasTensorIndex   = lastOp->inputs()->Get(2);

  // ── Extract weights ──
  const tflite::Tensor* weightTensor = subgraph->tensors()->Get(weightTensorIndex);
  const tflite::Buffer* weightBuffer = loadedModel->buffers()->Get(weightTensor->buffer());

  if(weightBuffer == nullptr || weightBuffer->data() == nullptr) {
    ESP_LOGE(TAG, "Weight buffer is empty!");
    initRandom(inSize, outSize);
    return;
  }

  const int8_t* rawWeights = reinterpret_cast<const int8_t*>(weightBuffer->data()->data());
  float wScale = weightTensor->quantization()->scale()->Get(0);
  int wZeroPoint = weightTensor->quantization()->zero_point()->Get(0);
  int numWeights = weightBuffer->data()->size();

  if(numWeights != inSize * outSize) {
    ESP_LOGE(TAG, "Weight size mismatch! Got %d, expected %d", numWeights, inSize * outSize);
    initRandom(inSize, outSize);
    return;
  }

  vector<float> loadedWeights(numWeights);
  for(int i = 0; i < numWeights; i++){
    loadedWeights[i] = (rawWeights[i] - wZeroPoint) * wScale;
  }
  weights = move(loadedWeights);
    // ── Extract biases ──
    const tflite::Tensor* biasTensor = subgraph->tensors()->Get(biasTensorIndex);
    const tflite::Buffer* biasBuffer = loadedModel->buffers()->Get(biasTensor->buffer());

    if (biasBuffer == nullptr || biasBuffer->data() == nullptr) {
        ESP_LOGE(TAG, "Bias buffer is empty!");
        biases.assign(outSize, 0.0f);
        return;
    }

    const int32_t* rawBiases = reinterpret_cast<const int32_t*>(biasBuffer->data()->data());
    float bScale     = biasTensor->quantization()->scale()->Get(0);
    int   bZeroPoint = biasTensor->quantization()->zero_point()->Get(0);
    int   numBiases  = biasBuffer->data()->size() / sizeof(int32_t);

    if (numBiases != outSize) {
        ESP_LOGE(TAG, "Bias size mismatch! Got %d, expected %d", numBiases, outSize);
        biases.assign(outSize, 0.0f);
        return;
    }

    vector<float> loadedBiases(numBiases);
    for (int i = 0; i < numBiases; i++)
        loadedBiases[i] = (rawBiases[i] - bZeroPoint) * bScale;
    biases = move(loadedBiases);

    ESP_LOGI(TAG, "Loaded last layer weights [%d] and biases [%d] from model", numWeights, numBiases);
}
// Initialize the layer with random values: use when not loading from flashed c-array model
void DenseLayer::initRandom(int inSize, int outSize){
  init(inSize, outSize);
  for(auto &w: weights){
    // Weights are randomized
    w = xavierRand(inputSize, outputSize);
    // biases are kept to 0
  }
}
// Create a method to pass forth the output...
vector<float> DenseLayer::forward(const vector<float> &input, bool isLastLayer){
  vector<float> output(outputSize);
    for (int o = 0; o < outputSize; o++) {
        float sum = biases[o];
        for (int i = 0; i < inputSize; i++)
            sum += weights[o * inputSize + i] * input[i];
        output[o] = isLastLayer ? sum : relu(sum);  // raw logits for last layer
    }
    if (isLastLayer) {
        // softmax
        float maxVal = *max_element(output.begin(), output.end());
        float sumExp = 0.0f;
        for (auto& v : output) { v = expf(v - maxVal); sumExp += v; }
        for (auto& v : output) v /= sumExp;
    }
    return output;
}
void DenseLayer::backward(const vector<float>& input, const vector<float>& output, const vector<float>& gradOutput) {
    timeStep++;
    float bc1 = 1.0f - powf(beta1, (float)timeStep);
    float bc2 = 1.0f - powf(beta2, (float)timeStep);

    for (int o = 0; o < outputSize; o++) {
      float actGrad = (output[o] > 0.0f) ? gradOutput[o] : 0.0f;
      
      // Reflect Adam Equation...
      // https://www.geeksforgeeks.org/deep-learning/adam-optimizer/
      m_b[o] = beta1 * m_b[o] + (1.0f - beta1) * actGrad;
      v_b[o] = beta2 * v_b[o] + (1.0f - beta2) * actGrad * actGrad;
      float m_b_hat = m_b[o] / bc1;
      float v_b_hat = v_b[o] / bc2;
      // Update the biases
      biases[o] -= learning_rate * m_b_hat / (sqrtf(v_b_hat) + epsilon);
      // Iteratively update the weights (each layer has 1 bias and multiple weights...)
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
// NVS Persistence: save to NVS
bool DenseLayer::saveToNVS(const char* key) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("ml_weights", NVS_READWRITE, &handle);
    if (err != ESP_OK) return false;

    // Save weights as a blob
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
void CustomHead::init(int featureSize){
  layer1.initFromLoadedModel(featureSize, 32);
  layer2.initRandom(32, 2);
  
  // Try loading from NVS; fall back to random init
  if (!layer1.loadFromNVS("l1")) layer1.initRandom(featureSize, 32);
  if (!layer2.loadFromNVS("l2")) layer2.initRandom(32, 2);
}
// Forward: forward through layer 1, then forward through layer 2...
vector<float> CustomHead::forward(const vector<float>& features) {
  vector<float> logits = layer1.forward(features);
  return layer2.forward(logits);
}
// Just calculate the overall loss, then backpropogate through each of the individual dense layers
void CustomHead::backward(const vector<float>& features, const vector<float>& probs, int label) {
  float loss = BCE_Loss(probs, label);
  vector<float> gradLayer2(layer2.outputSize);
  for (int o = 0; o < layer2.outputSize; o++){
    gradLayer2[o] = probs[o] - (o == label ? 1.0f : 0.0f);
  }

    vector<float> gradH(layer2.inputSize, 0.0f);
    for (int i = 0; i < layer2.inputSize; i++)
        for (int o = 0; o < layer2.outputSize; o++)
            gradH[i] += layer2.weights[o * layer2.inputSize + i] * gradLayer2[o];

    layer2.backward(lastHidden, probs, gradLayer2);
    layer1.backward(features, lastHidden, gradH);
}
// Just put the forward passes and backward passes together
void CustomHead::train(const std::vector<float>& features, int label) {
  auto probs = forward(features);   // caches lastHidden internally
  backward(features, probs, label); // uses lastHidden from cache
}
// Save the customHead: just save both its layers
void CustomHead::save() {
    layer1.saveToNVS("l1");
    layer2.saveToNVS("l2");
}
// Load the customHead: just load both its layers...
void CustomHead::load() {
    layer1.loadFromNVS("l1");
    layer2.loadFromNVS("l2");
}