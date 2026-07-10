#include <Arduino.h>
#include "board_config.h"

// Relay modules are active-LOW: HIGH = off, LOW = on (CLAUDE.md safety rules).
static const uint8_t RELAY_OFF = HIGH;
static const uint8_t RELAY_ON  = LOW;

static void relaysAllOff() {
  digitalWrite(PIN_RELAY_BURNER, RELAY_OFF);
  digitalWrite(PIN_RELAY_DRUM,   RELAY_OFF);
}

void setup() {
  Serial.begin(115200);
  delay(100);

  // Drive relays to the OFF level BEFORE switching the pins to OUTPUT, to
  // minimise a startup click while the GPIOs settle.
  digitalWrite(PIN_RELAY_BURNER, RELAY_OFF);
  digitalWrite(PIN_RELAY_DRUM,   RELAY_OFF);
  pinMode(PIN_RELAY_BURNER, OUTPUT);
  pinMode(PIN_RELAY_DRUM,   OUTPUT);
  relaysAllOff();

  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);

  Serial.println();
  Serial.println(F("[torrador] boot ok — V0 skeleton"));
  Serial.println(F("[torrador] target: ESP32"));
}

void loop() {
  // Single cooperative loop for V0 (no FreeRTOS tasks). The MAX6675 read
  // cycle, rule engine and web server come next.
  delay(1000);
}
