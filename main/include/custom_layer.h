// Plan: create a custom layer that stores an array of weights and biases
#pragma once 
#include <vector>
#include <cstdint>
#include "esp_log.h"
#include "nvs.h"
using namespace std;

// Global loss function
float BCE_Loss(const vector<float>& probs, int label);

// A single fully-connected layer with sigmoid output
// Input: float[inputSize]
// Output: float[outputSize]

struct DenseLayer{
  int inputSize;
  int outputSize;

  vector<float> weights; // [outputSize * inputSize], row-Major
  vector<float> biases; // [outputSize]

  // Part of Adam-style optimization...
  vector<float> m_w, v_w; // first/second moments for weights
  vector<float> m_b, v_b; // first/second moment for biases
  int timeStep = 0;
  
  float learning_rate = 0.001f;
  float beta1 = 0.9f;
  float beta2 = 0.999f;
  float epsilon = 1e-8f;

  void init(int inSize, int outSize);
  void initFromLoadedModel(int inSize, int outSize);
  void initRandom(int inSize, int outSize);
  vector<float> forward(const vector<float> &input, bool isLastLayer = false);
  void backward(const vector<float> &input, const vector<float> &output, const vector<float> &gradOutput);
  bool saveToNVS(const char* key);
  bool loadFromNVS(const char* key);
};

// The full custom head: 2 dense layers...
struct CustomHead{
  DenseLayer layer1;
  DenseLayer layer2;

  vector<float> lastHidden;

  void init(int featureSize);
  vector<float> forward(const vector<float> &features);
  void backward(const vector<float>& features, const vector<float>& probs, int label);
  void train(const vector<float> &features, int label);
  void save();
  void load();
};
