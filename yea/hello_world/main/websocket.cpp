// esp32s3_websocket_receiver.ino
#include <WiFi.h>
#include <ArduinoWebsockets.h>

using namespace websockets;

// ─── CONFIG ───────────────────────────────────────────────
const char* WIFI_SSID     = "Galaxy S20 5G 74b1";
const char* WIFI_PASSWORD = "helloooo";
const uint16_t WS_PORT    = 8765;
// ──────────────────────────────────────────────────────────

WebsocketsServer server;
WebsocketsClient client;
bool clientConnected = false;

// ── Stub: replace with your actual model inference call ───
void runInference(const char* data, size_t dataLen) {
  Serial.printf("[CV] Received frame: %u bytes\n", dataLen);

  // TODO: Pass data/dataLen into your flashed CV model here.
  // Example (replace with your model's actual API):
  //   cv_model_set_input((uint8_t*)data, dataLen);
  //   cv_model_run();
  //   int result = cv_model_get_output();
  //   Serial.printf("[CV] Inference result: %d\n", result);
}
// ──────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(500);

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

      // Notify laptop we're ready
      client.send("READY");

      // Callback for incoming messages
      client.onMessage([](WebsocketsMessage msg) {
        if (msg.isBinary()) {
          runInference(msg.c_str(), msg.length());
          // ACK back so laptop can pace next frame
          // (access via outer scope — use a flag if needed)
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

  // Poll connected client for new messages
  if (clientConnected && client.available()) {
    client.poll();

    // Send ACK after each poll cycle (paces the laptop sender)
    client.send("ACK");
  }
}