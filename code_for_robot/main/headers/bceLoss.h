// Necessary headers...
#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>
using namespace std;

double calculateBCELoss(const vector<double>& targets, const vector<double>& preds){
  // L = -[y * log(p) + (1-y) * log(1-p)]
  double totalLoss = 0.0;
  double epsilon = 1e-12;  // Avoid log(0)
  for(size_t i = 0; i < targets.size(); i++){
    // Clip predictions to prevent undefined log(0)
    double p = max(epsilon, min(1.0 - epsilon, preds[i]));
    double y = targets[i];
    totalLoss += -((y * log(p)) + (1 - y) * log(1 - p));
  }
  // Return average loss (that's how this works)
  return totalLoss/targets.size();
}
