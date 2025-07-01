#include <Arduino.h>

#define LED_BUILTIN 2 // On most ESP32 boards, GPIO2 is the onboard LED

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
    digitalWrite(LED_BUILTIN, HIGH);
    delay(500);
    digitalWrite(LED_BUILTIN, LOW);
    delay(500);
}
