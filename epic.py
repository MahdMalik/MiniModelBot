import flatbuffers
import tensorflow as tf

interpreter = tf.lite.Interpreter("tinyvgg_terrain_quantized_int8_w_head.tflite")
interpreter.allocate_tensors()

for detail in interpreter.get_tensor_details():
    print(detail['index'], detail['name'], detail['shape'], detail['dtype'])