#include "custom_layer.h"
#include <cmath>
#include <cstring>
#include <cstdlib>
#include "esp_log.h"
using namespace std;

static const char* TAG = "CUSTOM_LAYER";

CustomHead customHead;

// ─── Math helpers ────────────────────────────────────────────────────────────

static float sigmoid(float x) {
    return 1.0f / (1.0f + expf(-x));
}

static float relu(float x) {
    return x > 0.0f ? x : 0.0f;
}

// Xavier uniform initialization
static float xavierRand(int fanIn, int fanOut) {
    float limit = sqrtf(6.0f / (fanIn + fanOut));
    return ((float)rand() / RAND_MAX) * 2.0f * limit - limit;
}

// ─── DenseLayer ──────────────────────────────────────────────────────────────

void DenseLayer::init(int inSize, int outSize) {
    inputSize  = inSize;
    outputSize = outSize;

    weights.assign(outputSize * inputSize, 0.0f);
    biases.assign(outputSize, 0.0f);
    m_w.assign(outputSize * inputSize, 0.0f);
    v_w.assign(outputSize * inputSize, 0.0f);
    m_b.assign(outputSize, 0.0f);
    v_b.assign(outputSize, 0.0f);
}

void DenseLayer::initRandom() {
    for (auto& w : weights)
        w = xavierRand(inputSize, outputSize);
    // biases stay zero
}

vector<float> DenseLayer::forward(const vector<float>& input) {
    vector<float> out(outputSize, 0.0f);
    for (int o = 0; o < outputSize; o++) {
        float sum = biases[o];
        for (int i = 0; i < inputSize; i++)
            sum += weights[o * inputSize + i] * input[i];
        out[o] = relu(sum);  // use sigmoid in the final layer instead
    }
    return out;
}

// gradOutput: dLoss/dOutput for each output neuron
void DenseLayer::backward(const vector<float>& input,
                           const vector<float>& output,
                           const vector<float>& gradOutput) {
    timestep++;
    float bc1 = 1.0f - powf(beta1, timestep);
    float bc2 = 1.0f - powf(beta2, timestep);

    for (int o = 0; o < outputSize; o++) {
        // ReLU derivative
        float actGrad = (output[o] > 0.0f) ? gradOutput[o] : 0.0f;

        // Bias update (Adam)
        m_b[o] = beta1 * m_b[o] + (1 - beta1) * actGrad;
        v_b[o] = beta2 * v_b[o] + (1 - beta2) * actGrad * actGrad;
        biases[o] -= learningRate * (m_b[o] / bc1) / (sqrtf(v_b[o] / bc2) + epsilon);

        for (int i = 0; i < inputSize; i++) {
            float grad_w = actGrad * input[i];
            int idx = o * inputSize + i;

            // Weight update (Adam)
            m_w[idx] = beta1 * m_w[idx] + (1 - beta1) * grad_w;
            v_w[idx] = beta2 * v_w[idx] + (1 - beta2) * grad_w * grad_w;
            weights[idx] -= learningRate * (m_w[idx] / bc1) / (sqrtf(v_w[idx] / bc2) + epsilon);
        }
    }
}

// ─── NVS Persistence ─────────────────────────────────────────────────────────

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

// ─── CustomHead ──────────────────────────────────────────────────────────────

void CustomHead::init(int featureSize) {
    layer1.init(featureSize, 32);
    layer2.init(32, 2);  // 2 output classes

    // Try loading from NVS; fall back to random init
    if (!layer1.loadFromNVS("l1")) layer1.initRandom();
    if (!layer2.loadFromNVS("l2")) layer2.initRandom();
}

std::vector<float> CustomHead::forward(const std::vector<float>& features) {
    auto h = layer1.forward(features);

    // Final layer: sigmoid instead of relu for probabilities
    std::vector<float> out(layer2.outputSize, 0.0f);
    for (int o = 0; o < layer2.outputSize; o++) {
        float sum = layer2.biases[o];
        for (int i = 0; i < layer2.inputSize; i++)
            sum += layer2.weights[o * layer2.inputSize + i] * h[i];
        out[o] = sigmoid(sum);
    }
    return out;
}

void CustomHead::train(const std::vector<float>& features, int label) {
    // ── Forward pass ──
    auto h = layer1.forward(features);

    std::vector<float> logits(layer2.outputSize, 0.0f);
    for (int o = 0; o < layer2.outputSize; o++) {
        float sum = layer2.biases[o];
        for (int i = 0; i < layer2.inputSize; i++)
            sum += layer2.weights[o * layer2.inputSize + i] * h[i];
        logits[o] = sum;
    }

    // Softmax
    float maxLogit = *std::max_element(logits.begin(), logits.end());
    std::vector<float> probs(layer2.outputSize);
    float sumExp = 0.0f;
    for (int o = 0; o < layer2.outputSize; o++) {
        probs[o] = expf(logits[o] - maxLogit);
        sumExp += probs[o];
    }
    for (auto& p : probs) p /= sumExp;

    // ── Loss: cross-entropy gradient ──
    // dL/d(logit_o) = prob_o - 1{o == label}
    std::vector<float> gradLayer2(layer2.outputSize);
    for (int o = 0; o < layer2.outputSize; o++)
        gradLayer2[o] = probs[o] - (o == label ? 1.0f : 0.0f);

    // ── Backward: layer2 ──
    // Compute gradient w.r.t. layer1's output before layer2 backward modifies weights
    std::vector<float> gradH(layer2.inputSize, 0.0f);
    for (int i = 0; i < layer2.inputSize; i++)
        for (int o = 0; o < layer2.outputSize; o++)
            gradH[i] += layer2.weights[o * layer2.inputSize + i] * gradLayer2[o];

    layer2.backward(h, h /* unused for linear */, gradLayer2);

    // ── Backward: layer1 (ReLU grad applied inside backward()) ──
    layer1.backward(features, h, gradH);

    ESP_LOGI(TAG, "Trained on label %d | p[0]=%.3f p[1]=%.3f",
             label, probs[0], probs[1]);
}

void CustomHead::save() {
    layer1.saveToNVS("l1");
    layer2.saveToNVS("l2");
}

void CustomHead::load() {
    layer1.loadFromNVS("l1");
    layer2.loadFromNVS("l2");
}