#include <Arduino.h>
#include <FS.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include "debug_logger.h"
#include "env.h"
#include "uart_helpers.h"

// ---------------------- Configuration / Constants ----------------------
// Hardware
static const int LED_GPIO =
    2;  // On most ESP32 boards, GPIO2 is the onboard LED
static const int SERIAL1_RX_PIN = 16;  // UART RX for Serial1 (RP2040 -> ESP32)
static const int SERIAL1_TX_PIN = 17;  // UART TX for Serial1 (ESP32 -> RP2040)
static const int UART_BAUD_RATE = 115200;  // Standard UART baud used

// Image characteristics
static const uint32_t IMAGE_WIDTH = 800;
static const uint32_t IMAGE_HEIGHT = 480;
static const uint32_t IMAGE_BPP = 3;  // 3 bits per pixel for 7-color e-Paper
static const uint32_t IMAGE_SIZE =
    192000;  // 192,000 bytes for 800x480 (Waveshare)

// Networking / HTTP
static const char IMAGE_URL[] =
    "https://europe-north1-kitche-epaper-renderer.cloudfunctions.net/"
    "epaper?format=raw";
static const uint32_t HTTP_TIMEOUT_MS = 60000;  // 60s HTTP timeout

// Timeouts and retries
static const uint32_t WIFI_CONNECT_RETRIES = 60;  // 60 * 0.5s = 30s
static const uint32_t WIFI_RECONNECT_INTERVAL_MS = 30000;
static const uint32_t WIFI_RESET_DELAY_MS = 250;
static const uint32_t READ_TIMEOUT_MS = 30000;  // 30s stalled read timeout
static const int MAX_REDIRECTS = 3;

// Transfer parameters
static const size_t CHUNK_SIZE = 1024;  // 1KB per chunk
static const int MAX_ZERO_READS =
    100;  // max consecutive zero reads before giving up

// SOF marker used for framing the UART stream
static const uint8_t SOF_MARKER[4] = {0xAA, 0x55, 0xAA, 0x55};

// LED state tracking
unsigned long lastBlink = 0;
bool ledState = false;
bool spiffsReady = false;
String wifiSsid;
String wifiPass;
unsigned long lastWifiReconnectAttempt = 0;

bool ensure_spiffs_ready() {
  if (spiffsReady) {
    return true;
  }

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed!");
    return false;
  }

  spiffsReady = true;
  return true;
}

bool load_wifi_credentials() {
  if (wifiSsid.length() > 0 && wifiPass.length() > 0) {
    return true;
  }

  if (!ensure_spiffs_ready()) {
    return false;
  }

  wifiSsid = getEnvVar("/.env", "WIFI_SSID");
  wifiPass = getEnvVar("/.env", "WIFI_PASS");
  if (wifiSsid.length() == 0 || wifiPass.length() == 0) {
    Serial.println("WiFi credentials not found in /.env!");
    return false;
  }

  return true;
}

String getEnvVar(const char* filename, const char* key) {
  File f = SPIFFS.open(filename, "r");
  if (!f)
    return String("");
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.startsWith("#") || line.startsWith(";"))
      continue;
    int eq = line.indexOf('=');
    if (eq > 0) {
      String k = line.substring(0, eq);
      String v = line.substring(eq + 1);
      k.trim();
      v.trim();
      if (k == key) {
        f.close();
        return v;
      }
    }
  }
  f.close();
  return String("");
}

// Send two ACK lines with a short pause to make handshake more robust
void sendACKs(HardwareSerial& port) {
  port.print("ACK\n");
  delay(50);
  port.print("ACK\n");
}

// Write SOF marker and 4-byte big-endian image size header to the given serial
// port
void sendSOFHeader(HardwareSerial& port, uint32_t img_size) {
  port.write(SOF_MARKER, sizeof(SOF_MARKER));
  uint8_t header[4];
  buildImageHeader(img_size, header);
  port.write(header, 4);
}

bool connect_wifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  if (!load_wifi_credentials()) {
    return false;
  }

  Serial.print("Connecting to WiFi: ");
  Serial.println(wifiSsid);
  debug_log_connect_start(wifiSsid.c_str());
  lastWifiReconnectAttempt = millis();

  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.disconnect(true);
  delay(WIFI_RESET_DELAY_MS);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < WIFI_CONNECT_RETRIES) {
    delay(500);
    Serial.print(".");
    retries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    debug_log_connect_success(WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  } else {
    Serial.printf("\nWiFi connection failed! status=%d\n", WiFi.status());
    debug_log_connect_failed(WiFi.status(), retries);
    return false;
  }
}

void maintain_wifi_connection(unsigned long now) {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (now - lastWifiReconnectAttempt < WIFI_RECONNECT_INTERVAL_MS) {
    return;
  }

  Serial.println("WiFi disconnected; attempting background reconnect...");
  debug_log_reconnect_attempt();
  connect_wifi();
}

// NOTE: download_image_to_buffer was removed in favour of streaming directly
// from the HTTP response to the UART to avoid double-buffering large images.

void setup() {
  Serial.begin(UART_BAUD_RATE);  // USB serial for debug
  // Wait for serial connection
  while (!Serial) {
    delay(10);
  }
  pinMode(LED_GPIO, OUTPUT);
  // Initialize Serial1 for RP2040 UART
  Serial1.begin(UART_BAUD_RATE, SERIAL_8N1, SERIAL1_RX_PIN, SERIAL1_TX_PIN);
  Serial.println("ESP32 ready for stateless SENDIMG protocol");
  connect_wifi();
  debug_log_init();
  // Download test image at startup (optional, can be removed if not needed)
  // download_image_to_buffer("https://drive.google.com/uc?export=download&id=1vwwSW9DbbUZ_H7Q_CaS0rMNKG6HDWAHh");
}

void blink_slow() {
  unsigned long now = millis();
  if (now - lastBlink >= 500) {  // 1 Hz (slow)
    ledState = !ledState;
    digitalWrite(LED_GPIO, ledState);
    lastBlink = now;
  }
}

void blink_fast(unsigned long duration_ms) {
  unsigned long start = millis();
  while (millis() - start < duration_ms) {
    digitalWrite(LED_GPIO, HIGH);
    delay(50);
    digitalWrite(LED_GPIO, LOW);
    delay(50);
  }
}

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
  while (redirectCount < MAX_REDIRECTS) {
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setReuse(false);
    http.setUserAgent("ESP32/1.0");
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
    String payload = http.getString();
    Serial.print("HTTP error payload: ");
    Serial.println(payload);
    debug_log_http_error(currentUrl.c_str(), httpCode);
    http.end();
    return false;
  }
  WiFiClient* stream = http.getStreamPtr();
  // Send start-of-frame marker and header so the receiver can sync
  Serial.println("Sending SOF marker and header to receiver...");
  sendSOFHeader(port, IMAGE_SIZE);
  Serial.println("SOF marker and header sent");
  uint8_t chunk[CHUNK_SIZE];
  size_t received = 0;
  unsigned long lastData = millis();
  int zeroReadCount = 0;
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
        if (zeroReadCount > MAX_ZERO_READS) {
          Serial.println(
              "No more data from stream (max zero reads reached), breaking "
              "loop.");
          break;
        }
      } else {
        zeroReadCount = 0;
      }
      if (millis() - lastData > READ_TIMEOUT_MS) {
        Serial.println("Read timeout: no data received for 30s");
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
    debug_log_stream_error("incomplete transfer or timeout", received);
    return false;
  }
}

unsigned long lastStatusLog = 0;

void loop() {
  unsigned long now = millis();
  maintain_wifi_connection(now);

  // Listen for image request from RP2040 on Serial1
  if (Serial1.available()) {
    String cmd = Serial1.readStringUntil('\n');
    cmd += '\n';  // Ensure newline is included for exact match
    if (cmd == "GETLOG\n") {
      debug_log_dump_to_serial();
    } else if (cmd == "SENDIMG\n") {
      Serial.println("SENDIMG command received on Serial1");
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println(
            "WiFi disconnected; attempting reconnect before image fetch...");
        if (!connect_wifi()) {
          Serial.println(
              "ERROR: WiFi reconnect failed. Waiting for next request.");
          return;
        }
      }
      // Send two ACKs to ensure the RP2040 sees the handshake, then pause
      sendACKs(Serial1);
      Serial.println("ACKs sent to RP2040");
      delay(100);       // give the RP2040 time to process the ACKs before SOF
      blink_fast(600);  // Rapid blink for 600ms while sending
      bool ok = stream_image_to_uart(IMAGE_URL, Serial1);
      if (ok) {
        Serial.println("Image streamed to RP2040 on Serial1");
      } else {
        Serial.println(
            "ERROR: Image streaming to RP2040 failed. Waiting for next "
            "request.");
      }
    } else {
      Serial.printf("Unknown command: %s", cmd.c_str());
    }
  }
  // Status log every 5 seconds
  if (now - lastStatusLog >= 5000) {
    Serial.println("Waiting for SENDIMG request on Serial1...");
    lastStatusLog = now;
  }
  blink_slow();
  delay(10);
}
