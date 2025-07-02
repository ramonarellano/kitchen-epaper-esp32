#include <Arduino.h>
#include <WiFi.h>

#define LED_BUILTIN 2  // On most ESP32 boards, GPIO2 is the onboard LED
#define UART_BAUD 115200
#define UART_TX_PIN 1
#define UART_RX_PIN 3
#define IMAGE_WIDTH 800
#define IMAGE_HEIGHT 480
#define IMAGE_BPP 3  // 3 bits per pixel for 7-color e-Paper
#include "ImageData.h"
#define image_buffer Image7color
#undef IMAGE_SIZE
#define IMAGE_SIZE 192000  // 192,000 bytes for 800x480 (Waveshare)

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

void send_image_in_chunks(HardwareSerial& port, size_t total_size) {
  size_t sent = 0;
  size_t chunk_num = 0;
  Serial.println("Starting image transfer...");
  while (sent < total_size) {
    size_t to_send =
        (total_size - sent > CHUNK_SIZE) ? CHUNK_SIZE : (total_size - sent);
    port.write(image_buffer + sent, to_send);
    sent += to_send;
    chunk_num++;
    delay(10);  // Small delay to allow receiver to process
  }
  Serial.println("All chunks sent.");
}

void loop() {
  // Listen for image request from RP2040 on Serial1
  if (Serial1.available()) {
    String cmd = Serial1.readStringUntil('\n');
    cmd += '\n';  // Ensure newline is included for exact match
    if (cmd == "SENDIMG\n") {
      Serial.println("SENDIMG command received on Serial1");
      blink_fast(600);  // Rapid blink for 600ms while sending
      send_image_in_chunks(Serial1, IMAGE_SIZE);
      Serial.println("Image sent to RP2040 on Serial1 (chunked)");
    }
  }
  // Optionally, still allow USB serial monitor commands
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd += '\n';
    if (cmd == "SENDIMG\n") {
      Serial.println("SENDIMG command received on USB serial");
      blink_fast(600);
      send_image_in_chunks(Serial, IMAGE_SIZE);
      Serial.println("Image sent to USB serial (chunked)");
    }
  }
  blink_slow();
  delay(10);
}
