#pragma once

// -----------------------------------------------------------------------------
// board_config.h — ALL board/pin configuration lives here.
//
//   * WiFi / mDNS / async-transport includes
//   * Pin assignments
//
// Target board: ESP32-WROOM. Keep pin numbers and board includes confined to
// this file so the rest of the codebase stays hardware-agnostic.
//
// This file is the single source of truth for pin numbers. Physical wiring /
// connection tables live in docs/hardware/wiring.md — keep them in sync.
// -----------------------------------------------------------------------------

#include <Arduino.h>

#include <WiFi.h>
#include <ESPmDNS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>

// -----------------------------------------------------------------------------
// Pin map — PROVISIONAL. Final pin assignment is an open item in the PRD.
// Two MAX6675 share SCK + SO (read-only bus), each with its own CS.
// The relay module is active-LOW. Strapping/boot pins are avoided for outputs.
// -----------------------------------------------------------------------------

// MAX6675 thermocouple readers (shared SPI, individual CS).
static const uint8_t PIN_MAX6675_SCK   = 18;  // shared thermocouple clock
static const uint8_t PIN_MAX6675_SO    = 19;  // shared thermocouple data (MISO)
static const uint8_t PIN_MAX6675_CS_BT = 5;   // bean temperature (BT) chip-select
static const uint8_t PIN_MAX6675_CS_ET = 4;   // air/exhaust temperature (ET) chip-select (D4)

// SSD1306 0.96" OLED over I2C. These are the ESP32 default Wire pins, so the
// U8g2 hardware-I2C driver uses them without extra setup.
static const uint8_t PIN_OLED_SDA      = 21;  // I2C data
static const uint8_t PIN_OLED_SCL      = 22;  // I2C clock

// Gas flame controller (INV-27109) interface and provisioning button.
static const uint8_t PIN_INV_ENABLE    = 25;  // enables INV-27109 via mains relay; bench: LED indicator

// Drive polarity of PIN_INV_ENABLE. The relay module is active-LOW (a LOW input
// energises the coil, powering the INV). Set to true for an active-HIGH driver
// such as the bench LED. Never write HIGH/LOW to this pin directly — go through
// invEnableLevel() so swapping the driver stays a one-line change.
static const bool INV_ENABLE_ACTIVE_HIGH = false;

static inline uint8_t invEnableLevel(bool on) {
  return (on == INV_ENABLE_ACTIVE_HIGH) ? HIGH : LOW;
}
static const uint8_t PIN_FLAME_FAULT   = 32;  // INV fault via PC817 (active-LOW in final HW); bench: push-button (active-high)
static const uint8_t PIN_START_STOP    = 33;  // process start/stop toggle button (bench); active-high
static const uint8_t PIN_BOOT_BUTTON   = 0;   // BOOT button — lockout reset / WiFi credential reset
