#include <Arduino.h>

#define LED_BUILTIN 2  // On most ESP32 boards, GPIO2 is the onboard LED
#define UART_BAUD 115200
#define UART_TX_PIN 1
#define UART_RX_PIN 3
#define IMAGE_SIZE (800 * 480 / 8)  // 1bpp 800x480

// Handshake pins for Serial1
#define SERIAL1_RX 16
#define SERIAL1_TX 17

// 2x2 checkerboard pattern for 1bpp 800x480
uint8_t image_buffer[IMAGE_SIZE];

void fill_checkerboard() {
  // Each byte is 8 pixels, so for 800 pixels per row, 100 bytes per row
  // 2x2 checkerboard: alternate every 2 rows and every 2 columns
  for (int y = 0; y < 480; y++) {
    for (int x_byte = 0; x_byte < 100; x_byte++) {
      uint8_t pattern = 0;
      for (int bit = 0; bit < 8; bit++) {
        int x = x_byte * 8 + bit;
        bool is_white = (((x / 2) + (y / 2)) % 2) == 0;
        pattern |= (is_white ? 1 : 0) << (7 - bit);
      }
      image_buffer[y * 100 + x_byte] = pattern;
    }
  }
}

unsigned long lastBlink = 0;
bool ledState = false;
bool handshake_complete = false;

void setup() {
  Serial.begin(UART_BAUD);  // USB serial for debug
  // Wait for serial connection
  while (!Serial) {
    delay(10);
  }
  pinMode(LED_BUILTIN, OUTPUT);
  fill_checkerboard();

  // Initialize Serial1 for RP2040 handshake
  Serial1.begin(UART_BAUD, SERIAL_8N1, SERIAL1_RX, SERIAL1_TX);
  while (!handshake_complete) {
    Serial.println("Waiting for handshake…");
    unsigned long start = millis();
    String incoming = "";
    while (millis() - start < 2000) {
      while (Serial1.available()) {
        char c = Serial1.read();
        Serial.print("Serial1 got: ");
        Serial.print((int)c);  // Print ASCII code
        Serial.print(" ('");
        Serial.print(c);
        Serial.println("')");
        incoming += c;
        // More robust: match with or without newline
        if (incoming.indexOf("HELLO ESP32") != -1) {
          Serial.print("Received: ");
          Serial.println(incoming);
          Serial.println("Sending reply…");
          Serial1.println("HELLO RP2040");
          Serial.println("Handshake complete");
          handshake_complete = true;
          break;
        }
      }
      if (handshake_complete)
        break;
      delay(10);
    }
    if (!handshake_complete) {
      Serial.println("Handshake timeout, retrying...");
    }
  }
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

void loop() {
  if (!handshake_complete) {
    // Optionally, retry handshake here if desired
    return;
  }
  // Listen for image request from RP2040 on Serial1
  if (Serial1.available()) {
    String cmd = Serial1.readStringUntil('\n');
    cmd.trim();
    if (cmd == "SENDIMG") {
      blink_fast(600);  // Rapid blink for 600ms while sending
      Serial1.write(image_buffer, IMAGE_SIZE);
      Serial.println("Image sent to RP2040 on Serial1");
    }
  }
  // Optionally, still allow USB serial monitor commands
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "SENDIMG") {
      blink_fast(600);
      Serial.write(image_buffer, IMAGE_SIZE);
      Serial.println("Image sent to USB serial");
    }
  }
  blink_slow();
  delay(10);
}
