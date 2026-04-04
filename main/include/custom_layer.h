#pragma once
#include <vector>
#include <cstdint>
#include "esp_log.h" 
#include "nvs_flash.h"
#include "nvs.h"
using namespace std;

// A single fully-connected layer with sigmoid output
// Input:  float[inputSize]
// Output: float[outputSize]
struct DenseLayer{
  int inputSize;
  int outputSize;

  vector<float> weights; // [outputSize * inputSize], rowMajor
  vector<float> biases; // [outputSize]

  // Set up Adam optimizer state (in theory...)
  vector<float> m_w, v_w; // First/Second moments for weights
  vector<float> m_b, v_b; // First/Second moments for biases
  int timeStep = 0;

  float learningRate = 0.001f;
  float beta1 = 0.9f;
  float beta2 = 0.999f;
  float epsilon = 1e-8f;

  void init(int inSize, int outSize);
  void initRandom();
  vector<float> forward(const vector<float>& input);
  void backward(const vector<float>& input, 
    const vector<float>& output, 
    const vector<float>& gradOutput);
  bool saveToNVS(const char* key);
  bool loadFromNVS(const char* key);
}
struct CustomHead{
  DenseLayer denselayer1;  // E.g.: 128 -> 32
  DenseLayer denselayer2;  // E.g.: 32 -> 2

  void init(int featureSize);
  vector<float> forward(const vector<float>& features, int label);
  void save();
  void load();
}