#include <Arduino.h>
#include <FS.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>
#include "debug_logger.h"
#include "env.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "uart_helpers.h"

// ---------------------- Configuration / Constants ----------------------
// Hardware
static const int LED_GPIO =
    2;  // On most ESP32 boards, GPIO2 is the onboard LED
static const int SERIAL1_RX_PIN = 16;  // UART RX for Serial1 (RP2040 -> ESP32)
static const int SERIAL1_TX_PIN = 17;  // UART TX for Serial1 (ESP32 -> RP2040)
static const int PICO_POWER_PIN = 25;  // GPIO to control Pico power via MOSFET
static const int UART_BAUD_RATE = 115200;  // Standard UART baud used

// Timing
static const unsigned long UPDATE_INTERVAL_MS =
    5UL * 60UL * 1000UL;  // 5 minutes between display update cycles
static const unsigned long PICO_BOOT_SETTLE_MS =
    3000;  // Wait for Pico to boot after power-on
static const unsigned long PICO_DONE_TIMEOUT_MS =
    120000;  // 120s max for full Pico cycle (transfer + display)

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

// WiFi recovery
static const int MAX_CONSECUTIVE_WIFI_FAILURES = 3;

// Track whether WiFi was intentionally started (to prevent maintain_wifi from
// reconnecting while we want the radio off during idle wait).
bool wifiActivated = false;

// LED state tracking
unsigned long lastBlink = 0;
bool ledState = false;
bool spiffsReady = false;
String wifiSsid;
String wifiPass;
unsigned long lastWifiReconnectAttempt = 0;
int consecutiveWifiFailures = 0;

bool ensure_spiffs_ready() {
  if (spiffsReady) {
    return true;
  }

  // Do not auto-format on mount failure: preserving persisted logs is safer
  // than recovering a damaged filesystem by erasing it.
  if (!SPIFFS.begin(false)) {
    Serial.println("SPIFFS mount failed (no auto-format).");
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
    consecutiveWifiFailures = 0;

    // Sync real-time clock via NTP so log timestamps are meaningful
    // Oslo timezone: CET (UTC+1) with CEST (UTC+2) daylight saving
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5000)) {  // 5s timeout
      char timebuf[64];
      strftime(timebuf, sizeof(timebuf), "NTP time: %Y-%m-%d %H:%M:%S Oslo",
               &timeinfo);
      debug_log_event(timebuf);
    } else {
      debug_log_event("NTP sync failed (5s timeout)");
    }

    return true;
  } else {
    Serial.printf("\nWiFi connection failed! status=%d\n", WiFi.status());
    debug_log_connect_failed(WiFi.status(), retries);
    consecutiveWifiFailures++;
    if (consecutiveWifiFailures >= MAX_CONSECUTIVE_WIFI_FAILURES) {
      debug_log_event("Too many consecutive WiFi failures, rebooting ESP32");
      delay(100);
      ESP.restart();
    }
    return false;
  }
}

// NOTE: download_image_to_buffer was removed in favour of streaming directly
// from the HTTP response to the UART to avoid double-buffering large images.

void setup() {
  Serial.begin(UART_BAUD_RATE);  // USB serial for debug
  // Wait for serial connection (with timeout for headless operation)
  unsigned long serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 3000) {
    delay(10);
  }
  pinMode(LED_GPIO, OUTPUT);
  // Pico power control — start with Pico OFF
  pinMode(PICO_POWER_PIN, OUTPUT);
  digitalWrite(PICO_POWER_PIN, LOW);
  // Initialize Serial1 for RP2040 UART
  Serial1.begin(UART_BAUD_RATE, SERIAL_8N1, SERIAL1_RX_PIN, SERIAL1_TX_PIN);
  Serial.println("ESP32 ready — power-cycling master mode");
  debug_log_init();
  WiFi.mode(WIFI_OFF);
}

// -------------- Pico power helpers -------------- //

void pico_power_on() {
  Serial.println("Powering ON Pico");
  debug_log_event("PICO_POWER_ON");
  digitalWrite(PICO_POWER_PIN, HIGH);
}

void pico_power_off() {
  Serial.println("Powering OFF Pico");
  debug_log_event("PICO_POWER_OFF");
  digitalWrite(PICO_POWER_PIN, LOW);
}

// Drain and store any PLOG lines from the Pico on Serial1.
// Returns true if a SENDIMG command was seen.
bool drain_pico_lines(bool store_plogs) {
  bool saw_sendimg = false;
  while (Serial1.available()) {
    String line = Serial1.readStringUntil('\n');
    line.trim();
    if (line.length() == 0)
      continue;
    if (line.startsWith("PLOG:") && store_plogs) {
      String msg = line.substring(5);
      msg.trim();
      if (msg.length() > 0) {
        debug_log_event("[PICO]", msg.c_str());
      }
    } else if (line == "SENDIMG") {
      saw_sendimg = true;
    } else if (line == "PICODONE") {
      // handled by caller
    } else {
      Serial.printf("Pico line: %s\n", line.c_str());
    }
  }
  return saw_sendimg;
}

// Wait for a specific command from the Pico, while storing PLOG lines.
// Returns true if the command was received, false on timeout.
bool wait_for_pico_command(const char* cmd, unsigned long timeout_ms) {
  unsigned long start = millis();
  while (millis() - start < timeout_ms) {
    if (Serial1.available()) {
      String line = Serial1.readStringUntil('\n');
      line.trim();
      if (line.length() == 0)
        continue;
      if (line.startsWith("PLOG:")) {
        String msg = line.substring(5);
        msg.trim();
        if (msg.length() > 0) {
          debug_log_event("[PICO]", msg.c_str());
        }
      } else if (line == cmd) {
        return true;
      } else {
        Serial.printf("Pico line (waiting for %s): %s\n", cmd, line.c_str());
      }
    }
    delay(10);
  }
  return false;
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
  WiFiClientSecure secureClient;
  secureClient.setInsecure();  // skip cert pin for cloud function endpoint
  int httpCode = 0;
  while (redirectCount < MAX_REDIRECTS) {
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setReuse(false);
    http.setUserAgent("ESP32/1.0");
    http.begin(secureClient, currentUrl);
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
  // Validate Content-Length before committing to the transfer (Bug #9 fix).
  // If the renderer returns an error page or wrong-sized response, bail out
  // before sending SOF — otherwise the RP2040 waits 3 minutes for data that
  // will never arrive.
  int contentLength = http.getSize();
  if (contentLength > 0 && (uint32_t)contentLength != IMAGE_SIZE) {
    Serial.printf("Unexpected Content-Length: %d (expected %u)\n",
                  contentLength, (unsigned)IMAGE_SIZE);
    debug_log_event("Content-Length mismatch, aborting transfer");
    http.end();
    return false;
  }
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
static unsigned long cycleCount = 0;

void loop() {
  unsigned long now = millis();

  // Handle USB serial commands (GETLOG/CLEARLOG) while idle
  if (Serial.available()) {
    String usbCmd = Serial.readStringUntil('\n');
    usbCmd.trim();
    if (usbCmd == "GETLOG") {
      debug_log_dump_to_serial();
    } else if (usbCmd == "CLEARLOG") {
      debug_log_clear();
      Serial.println("[DEBUG] Persistent log cleared");
    } else if (usbCmd.length() > 0) {
      Serial.printf("Unknown USB command: %s\n", usbCmd.c_str());
    }
  }

  // ---- Power-cycling master loop ----
  // 1. Power on the Pico
  cycleCount++;
  char cycleBuf[64];
  snprintf(cycleBuf, sizeof(cycleBuf), "CYCLE_START #%lu", cycleCount);
  debug_log_event(cycleBuf);

  pico_power_on();
  delay(PICO_BOOT_SETTLE_MS);  // let Pico boot and flush PLOG

  // 2. Wait for SENDIMG from Pico (it sends PLOG lines first, then SENDIMG)
  Serial.println("Waiting for SENDIMG from Pico...");
  bool got_sendimg = wait_for_pico_command("SENDIMG", 30000);  // 30s timeout
  if (!got_sendimg) {
    debug_log_event("SENDIMG not received from Pico within 30s");
    Serial.println("ERROR: No SENDIMG from Pico — powering off");
    pico_power_off();
    debug_log_event("CYCLE_FAIL", "no SENDIMG");
    goto idle_wait;
  }

  debug_log_event("SENDIMG command received", "source=Serial1");
  Serial.println("SENDIMG received — connecting WiFi and fetching image");

  // 3. Connect WiFi and stream image
  {
    wifiActivated = true;
    if (!connect_wifi()) {
      Serial.println("ERROR: WiFi connect failed");
      debug_log_event("SENDIMG aborted", "wifi connect failed");
      // Send a minimal response so Pico doesn't hang forever
      pico_power_off();
      wifiActivated = false;
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      debug_log_event("CYCLE_FAIL", "wifi");
      goto idle_wait;
    }

    sendACKs(Serial1);
    Serial.println("ACKs sent to Pico");
    delay(100);
    blink_fast(600);

    bool ok = stream_image_to_uart(IMAGE_URL, Serial1);
    // Disconnect WiFi immediately after transfer
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiActivated = false;

    if (ok) {
      Serial.println("Image streamed to Pico");
      debug_log_event("Image streamed successfully", "target=Serial1");
    } else {
      Serial.println("ERROR: Image streaming failed");
      debug_log_event("Image streaming failed", "target=Serial1");
      pico_power_off();
      debug_log_event("CYCLE_FAIL", "stream");
      goto idle_wait;
    }
  }

  // 4. Wait for PICODONE (Pico has displayed image and parked the panel)
  Serial.println("Waiting for PICODONE from Pico...");
  {
    bool got_done = wait_for_pico_command("PICODONE", PICO_DONE_TIMEOUT_MS);
    if (got_done) {
      debug_log_event("PICODONE received — cycle complete");
      Serial.println("PICODONE received");
    } else {
      debug_log_event("PICODONE timeout — forcing power off");
      Serial.println("WARNING: PICODONE not received within timeout");
    }
  }

  // 5. Power off the Pico
  pico_power_off();
  debug_log_event("CYCLE_DONE");

idle_wait:
  // 6. Wait for the next cycle
  {
    char hb[80];
    snprintf(hb, sizeof(hb), "IDLE_WAIT %lus until next cycle",
             UPDATE_INTERVAL_MS / 1000);
    debug_log_event(hb);
    Serial.printf("Waiting %lu seconds until next cycle...\n",
                  UPDATE_INTERVAL_MS / 1000);

    unsigned long waitStart = millis();
    while (millis() - waitStart < UPDATE_INTERVAL_MS) {
      // Handle USB serial commands during idle
      if (Serial.available()) {
        String usbCmd = Serial.readStringUntil('\n');
        usbCmd.trim();
        if (usbCmd == "GETLOG") {
          debug_log_dump_to_serial();
        } else if (usbCmd == "CLEARLOG") {
          debug_log_clear();
          Serial.println("[DEBUG] Persistent log cleared");
        }
      }
      // Periodic status
      now = millis();
      if (now - lastStatusLog >= 60000) {
        unsigned long remaining = UPDATE_INTERVAL_MS - (now - waitStart);
        Serial.printf("Idle: %lu seconds remaining\n", remaining / 1000);
        lastStatusLog = now;
      }
      blink_slow();
      delay(10);
    }
  }
}
