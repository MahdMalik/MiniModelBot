// esp32s3_websocket_receiver.ino
#include <WiFi.h>
#include <ArduinoWebsockets.h>

// TFLite Micro
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

// JPEG decoder
#include "jpeg_decoder.h"

// Model weights
#include "model_weights.h"

using namespace websockets;

// ─── CONFIG ───────────────────────────────────────────────────────────────────
const char* WIFI_SSID     = "Galaxy S20 5G 74b1";
const char* WIFI_PASSWORD = "helloooo";
const uint16_t WS_PORT    = 8765;
// ──────────────────────────────────────────────────────────────────────────────

// ─── MODEL CONFIG ─────────────────────────────────────────────────────────────
#define MODEL_WIDTH       64
#define MODEL_HEIGHT      64
#define MODEL_CHANNELS    3
#define NUM_CLASSES       4
#define TENSOR_ARENA_SIZE (96 * 1024)

// Edit these to match your actual training labels
static const char* CLASS_LABELS[NUM_CLASSES] = {
    "class_0",
    "class_1",
    "class_2",
    "class_3",
};
// ──────────────────────────────────────────────────────────────────────────────

static uint8_t s_tensor_arena[TENSOR_ARENA_SIZE];
static tflite::MicroInterpreter* s_interpreter = nullptr;
static TfLiteTensor* s_input  = nullptr;
static TfLiteTensor* s_output = nullptr;

WebsocketsServer server;
WebsocketsClient client;
bool clientConnected = false;

// ─────────────────────────────────────────────────────────────────────────────
// cv_model_set_input: decode the incoming JPEG, resize to 64x64, and load
// the pixel data into the TFLite input tensor ready for inference.
// ─────────────────────────────────────────────────────────────────────────────
static bool cv_model_set_input(const uint8_t* jpeg_data, size_t jpeg_len)
{
    // 1. Get the decoded image dimensions without fully decoding yet,
    //    so we can allocate exactly the right output buffer.
    esp_jpeg_image_cfg_t info_cfg = {};
    info_cfg.indata      = (uint8_t*)jpeg_data;
    info_cfg.indata_size = jpeg_len;
    info_cfg.out_format  = JPEG_IMAGE_FORMAT_RGB888;
    info_cfg.out_scale   = JPEG_IMAGE_SCALE_0;

    esp_jpeg_image_output_t info = {};
    if (esp_jpeg_get_image_info(&info_cfg, &info) != ESP_OK) {
        Serial.println("[CV] Failed to read JPEG info");
        return false;
    }

    // 2. Allocate output buffer and decode.
    uint8_t* rgb = (uint8_t*)malloc(info.output_len);
    if (!rgb) {
        Serial.println("[CV] OOM for JPEG decode buffer");
        return false;
    }

    esp_jpeg_image_cfg_t dec_cfg = {};
    dec_cfg.indata      = (uint8_t*)jpeg_data;
    dec_cfg.indata_size = jpeg_len;
    dec_cfg.outbuf      = rgb;
    dec_cfg.outbuf_size = info.output_len;
    dec_cfg.out_format  = JPEG_IMAGE_FORMAT_RGB888;
    dec_cfg.out_scale   = JPEG_IMAGE_SCALE_0;

    esp_jpeg_image_output_t decoded = {};
    if (esp_jpeg_decode(&dec_cfg, &decoded) != ESP_OK) {
        Serial.println("[CV] JPEG decode failed");
        free(rgb);
        return false;
    }

    uint32_t src_w = decoded.width;
    uint32_t src_h = decoded.height;
    Serial.printf("[CV] Decoded JPEG: %ux%u\n", src_w, src_h);

    // 3. Nearest-neighbour resize to MODEL_WIDTH x MODEL_HEIGHT
    //    and fill the input tensor (handles both int8 and uint8 quantised models).
    bool  is_int8 = (s_input->type == kTfLiteInt8);
    float scale   = s_input->params.scale;
    int   zp      = s_input->params.zero_point;

    for (int y = 0; y < MODEL_HEIGHT; y++) {
        uint32_t sy = (uint32_t)((float)y / MODEL_HEIGHT * src_h);
        for (int x = 0; x < MODEL_WIDTH; x++) {
            uint32_t sx  = (uint32_t)((float)x / MODEL_WIDTH * src_w);
            int      src = (sy * src_w + sx) * 3;
            int      dst = (y  * MODEL_WIDTH + x) * MODEL_CHANNELS;

            for (int c = 0; c < MODEL_CHANNELS; c++) {
                float px = (float)rgb[src + c];
                if (is_int8) {
                    int q = (int)(px / scale) + zp;
                    if (q >  127) q =  127;
                    if (q < -128) q = -128;
                    s_input->data.int8[dst + c] = (int8_t)q;
                } else {
                    s_input->data.uint8[dst + c] = (uint8_t)px;
                }
            }
        }
    }

    free(rgb);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// cv_model_run: run inference on whatever is already in the input tensor.
// Returns true on success.
// ─────────────────────────────────────────────────────────────────────────────
static bool cv_model_run()
{
    if (s_interpreter->Invoke() != kTfLiteOk) {
        Serial.println("[CV] Invoke() failed");
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// cv_model_get_output: argmax over the output tensor scores.
// Returns the winning class index (0 to NUM_CLASSES-1).
// ─────────────────────────────────────────────────────────────────────────────
static int cv_model_get_output()
{
    int   best   = 0;
    float bscore = -1e9f;

    for (int i = 0; i < NUM_CLASSES; i++) {
        float score = (s_output->type == kTfLiteInt8)
            ? (s_output->data.int8[i] - s_output->params.zero_point)
              * s_output->params.scale
            : s_output->data.f[i];

        Serial.printf("[CV]   class %d (%s) = %.4f\n", i, CLASS_LABELS[i], score);
        if (score > bscore) { bscore = score; best = i; }
    }

    Serial.printf("[CV] ► Prediction: %s (%.4f)\n", CLASS_LABELS[best], bscore);
    return best;
}

// ─────────────────────────────────────────────────────────────────────────────
// TFLite initialisation — called once from setup()
// ─────────────────────────────────────────────────────────────────────────────
static bool tflite_init()
{
    tflite::InitializeTarget();

    const tflite::Model* model = tflite::GetModel(g_model2_quantized);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        Serial.printf("[TF] Schema mismatch: got %u expected %d\n",
                      model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }

    static tflite::MicroMutableOpResolver<10> resolver;
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddFullyConnected();
    resolver.AddAveragePool2D();
    resolver.AddSoftmax();
    resolver.AddReshape();
    resolver.AddAdd();
    resolver.AddMul();
    resolver.AddQuantize();
    resolver.AddDequantize();

    static tflite::MicroInterpreter interpreter(
        model, resolver, s_tensor_arena, TENSOR_ARENA_SIZE);

    if (interpreter.AllocateTensors() != kTfLiteOk) {
        Serial.println("[TF] AllocateTensors() failed");
        return false;
    }

    s_interpreter = &interpreter;
    s_input       = interpreter.input(0);
    s_output      = interpreter.output(0);

    Serial.printf("[TF] Ready. Input [%d,%d,%d,%d] type=%d\n",
                  s_input->dims->data[0], s_input->dims->data[1],
                  s_input->dims->data[2], s_input->dims->data[3],
                  s_input->type);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// runInference — called on every incoming binary WebSocket frame
// ─────────────────────────────────────────────────────────────────────────────
void runInference(const char* data, size_t dataLen) {
    Serial.printf("[CV] Received frame: %u bytes\n", dataLen);

    if (!cv_model_set_input((const uint8_t*)data, dataLen)) {
        client.send("{\"error\":\"set_input failed\"}");
        return;
    }

    if (!cv_model_run()) {
        client.send("{\"error\":\"inference failed\"}");
        return;
    }

    int result = cv_model_get_output();

    // Send result back to the Python script as JSON
    char json[128];
    snprintf(json, sizeof(json),
             "{\"class\":%d,\"label\":\"%s\"}", result, CLASS_LABELS[result]);
    client.send(json);
    Serial.printf("[WS] Sent: %s\n", json);
}

// ─────────────────────────────────────────────────────────────────────────────
// setup / loop — unchanged from original
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);

    // Initialise TFLite before anything else
    Serial.println("[TF] Initialising TFLite Micro...");
    if (!tflite_init()) {
        Serial.println("[TF] FAILED — halting.");
        while (1) delay(1000);
    }

    // Connect to WiFi
    Serial.printf("\nConnecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\n[WiFi] Connected! IP: %s\n",
                  WiFi.localIP().toString().c_str());

    // Start WebSocket server
    server.listen(WS_PORT);
    if (server.available()) {
        Serial.printf("[WS] Server started on port %u\n", WS_PORT);
    } else {
        Serial.println("[WS] ERROR: Server failed to start!");
    }
}

void loop() {
    // Accept new client if none connected
    if (!clientConnected && server.poll()) {
        client = server.accept();

        if (client.available()) {
            clientConnected = true;
            Serial.println("[WS] Client connected!");

            client.send("READY");

            client.onMessage([](WebsocketsMessage msg) {
                if (msg.isBinary()) {
                    runInference(msg.c_str(), msg.length());
                } else if (msg.isText()) {
                    Serial.printf("[WS] Text: %s\n", msg.c_str());
                }
            });

            client.onEvent([](WebsocketsEvent event, String data) {
                if (event == WebsocketsEvent::ConnectionClosed) {
                    Serial.println("[WS] Client disconnected.");
                    clientConnected = false;
                }
            });
        }
    }

    if (clientConnected && client.available()) {
        client.poll();
    }
}