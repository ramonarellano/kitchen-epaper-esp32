#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#define LED_BUILTIN 2  // On most ESP32 boards, GPIO2 is the onboard LED
#define UART_BAUD 115200
#define UART_TX_PIN 1
#define UART_RX_PIN 3
#define IMAGE_WIDTH 800
#define IMAGE_HEIGHT 480
#define IMAGE_BPP 3  // 3 bits per pixel for 7-color e-Paper
#undef IMAGE_SIZE
#define IMAGE_SIZE 192000         // 192,000 bytes for 800x480 (Waveshare)
uint8_t* image_buffer = nullptr;  // Will be allocated in PSRAM

// Handshake pins for Serial1
#define SERIAL1_RX 16
#define SERIAL1_TX 17

// 7-color e-Paper color codes (3 bits per pixel)
#define EPD_BLACK 0b000
#define EPD_WHITE 0b001
#define EPD_GREEN 0b010
#define EPD_BLUE 0b011
#define EPD_RED 0b100
#define EPD_YELLOW 0b101
#define EPD_ORANGE 0b110
#define EPD_UNUSED 0b111

// WiFi credentials (replace with your actual SSID and password)
const char* WIFI_SSID = "arelor";
const char* WIFI_PASS = "brumlurentser";

unsigned long lastBlink = 0;
bool ledState = false;

void connect_wifi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 60) {  // 30s timeout
    delay(500);
    Serial.print(".");
    retries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connection failed!");
  }
}

// Download image from the internet and fill image_buffer
bool download_image_to_buffer(const char* url) {
  if (!image_buffer) {
    Serial.println("Image buffer not allocated!");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, cannot download image.");
    return false;
  }
  HTTPClient http;
  Serial.print("Downloading image: ");
  Serial.println(url);
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.print("HTTP GET failed, code: ");
    Serial.println(httpCode);
    http.end();
    return false;
  }
  WiFiClient* stream = http.getStreamPtr();
  size_t received = 0;
  while (http.connected() && received < IMAGE_SIZE) {
    if (stream->available()) {
      int c = stream->read();
      if (c < 0)
        break;
      image_buffer[received++] = (uint8_t)c;
      if (received % 4096 == 0) {
        Serial.printf("Downloaded %u/%u bytes\n", (unsigned)received,
                      (unsigned)IMAGE_SIZE);
      }
    } else {
      delay(1);
    }
  }
  http.end();
  if (received == IMAGE_SIZE) {
    Serial.println("Image download complete!");
    return true;
  } else {
    Serial.printf("Image download incomplete: %u/%u bytes\n",
                  (unsigned)received, (unsigned)IMAGE_SIZE);
    return false;
  }
}

void setup() {
  Serial.begin(UART_BAUD);  // USB serial for debug
  // Wait for serial connection
  while (!Serial) {
    delay(10);
  }
  pinMode(LED_BUILTIN, OUTPUT);
  // Initialize Serial1 for RP2040 UART
  Serial1.begin(UART_BAUD, SERIAL_8N1, SERIAL1_RX, SERIAL1_TX);
  Serial.println("ESP32 ready for stateless SENDIMG protocol");
  connect_wifi();
  // Download test image at startup (optional, can be removed if not needed)
  // download_image_to_buffer("https://drive.google.com/uc?export=download&id=1vwwSW9DbbUZ_H7Q_CaS0rMNKG6HDWAHh");
}

void blink_slow() {
  unsigned long now = millis();
  if (now - lastBlink >= 500) {  // 1 Hz (slow)
    ledState = !ledState;
    digitalWrite(LED_BUILTIN, ledState);
    lastBlink = now;
  }
}

void blink_fast(unsigned long duration_ms) {
  unsigned long start = millis();
  while (millis() - start < duration_ms) {
    digitalWrite(LED_BUILTIN, HIGH);
    delay(50);
    digitalWrite(LED_BUILTIN, LOW);
    delay(50);
  }
}

#define CHUNK_SIZE 1024  // 1KB per chunk, adjust as needed

// Stream image from HTTP directly to UART
bool stream_image_to_uart(const char* url, HardwareSerial& port) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, cannot stream image.");
    return false;
  }
  String currentUrl = url;
  int redirectCount = 0;
  const int maxRedirects = 3;
  HTTPClient http;
  int httpCode = 0;
  while (redirectCount < maxRedirects) {
    http.setTimeout(30000);  // 30s timeout for large images
    http.begin(currentUrl);
    httpCode = http.GET();
    Serial.printf("HTTP GET %s -> code: %d\n", currentUrl.c_str(), httpCode);
    // Print HTTP headers for diagnostics
    int numHeaders = http.headers();
    for (int i = 0; i < numHeaders; ++i) {
      Serial.printf("Header[%d]: %s: %s\n", i, http.headerName(i).c_str(),
                    http.header(i).c_str());
    }
    if (httpCode >= 300 && httpCode < 400) {
      String location = http.header("Location");
      Serial.print("Redirected to: ");
      Serial.println(location);
      http.end();
      if (location.length() == 0) {
        Serial.println("Redirect with no Location header!");
        return false;
      }
      currentUrl = location;
      redirectCount++;
      continue;
    } else {
      break;
    }
  }
  if (httpCode != HTTP_CODE_OK) {
    Serial.print("HTTP GET failed, code: ");
    Serial.println(httpCode);
    http.end();
    return false;
  }
  WiFiClient* stream = http.getStreamPtr();
  uint8_t chunk[CHUNK_SIZE];
  size_t received = 0;
  unsigned long lastData = millis();
  const unsigned long readTimeout = 10000;  // 10s timeout for stalled reads
  int zeroReadCount = 0;
  const int maxZeroReads = 100;  // Allow up to 100 consecutive zero-byte reads
  while (http.connected() && received < IMAGE_SIZE) {
    size_t to_read = (IMAGE_SIZE - received > CHUNK_SIZE)
                         ? CHUNK_SIZE
                         : (IMAGE_SIZE - received);
    int n = stream->readBytes(chunk, to_read);
    Serial.printf("readBytes returned %d (requested %u) at %u/%u bytes\n", n,
                  (unsigned)to_read, (unsigned)received, (unsigned)IMAGE_SIZE);
    if (n > 0) {
      port.write(chunk, n);
      received += n;
      lastData = millis();
      zeroReadCount = 0;
      if (received % (CHUNK_SIZE * 4) == 0) {
        Serial.printf("Streamed %u/%u bytes\n", (unsigned)received,
                      (unsigned)IMAGE_SIZE);
      }
    } else {
      if (stream->available() == 0) {
        zeroReadCount++;
        if (zeroReadCount > maxZeroReads) {
          Serial.println(
              "No more data from stream (max zero reads reached), breaking "
              "loop.");
          break;
        }
      } else {
        zeroReadCount = 0;
      }
      if (millis() - lastData > readTimeout) {
        Serial.println("Read timeout: no data received for 10s");
        break;
      }
      delay(10);
    }
  }
  http.end();
  if (received == IMAGE_SIZE) {
    Serial.println("Image streaming complete!");
    return true;
  } else {
    Serial.printf("Image streaming incomplete: %u/%u bytes\n",
                  (unsigned)received, (unsigned)IMAGE_SIZE);
    return false;
  }
}

unsigned long lastStatusLog = 0;

void loop() {
  // Listen for image request from RP2040 on Serial1
  if (Serial1.available()) {
    String cmd = Serial1.readStringUntil('\n');
    cmd += '\n';  // Ensure newline is included for exact match
    if (cmd == "SENDIMG\n") {
      Serial.println("SENDIMG command received on Serial1");
      blink_fast(600);  // Rapid blink for 600ms while sending
      bool ok = stream_image_to_uart(
          "https://raw.githubusercontent.com/ramonarellano/emilies-wishlist/"
          "main/test_image.bmp",
          Serial1);
      if (ok) {
        Serial.println("Image streamed to RP2040 on Serial1");
      } else {
        Serial.println(
            "ERROR: Image streaming to RP2040 failed. Waiting for next "
            "request.");
      }
    }
  }
  // Optionally, still allow USB serial monitor commands
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd += '\n';
    if (cmd == "SENDIMG\n") {
      Serial.println("SENDIMG command received on USB serial");
      blink_fast(600);
      bool ok = stream_image_to_uart(
          "https://raw.githubusercontent.com/ramonarellano/emilies-wishlist/"
          "main/test_image.bmp",
          Serial);
      if (ok) {
        Serial.println("Image streamed to USB serial");
      } else {
        Serial.println(
            "ERROR: Image streaming to USB serial failed. Waiting for next "
            "request.");
      }
    }
  }
  // Status log every 5 seconds
  unsigned long now = millis();
  if (now - lastStatusLog >= 5000) {
    Serial.println("Waiting for SENDIMG request on Serial1 or USB serial...");
    lastStatusLog = now;
  }
  blink_slow();
  delay(10);
}
