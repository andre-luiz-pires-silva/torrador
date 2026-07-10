#pragma once

// -----------------------------------------------------------------------------
// board_config.h — ALL board/pin configuration lives here.
//
//   * WiFi / mDNS / async-transport includes
//   * Pin assignments
//
// Target board: ESP32-WROOM. Keep pin numbers and board includes confined to
// this file so the rest of the codebase stays hardware-agnostic.
// -----------------------------------------------------------------------------

#include <Arduino.h>

#include <WiFi.h>
#include <ESPmDNS.h>
#include <AsyncTCP.h>

// -----------------------------------------------------------------------------
// Pin map — PROVISIONAL. Final pin assignment is an open item in the PRD.
// Two MAX6675 share SCK + SO (read-only bus), each with its own CS.
// Relay modules are active-LOW. Strapping/boot pins are avoided for outputs.
// -----------------------------------------------------------------------------
static const uint8_t PIN_MAX6675_SCK   = 18;  // shared thermocouple clock
static const uint8_t PIN_MAX6675_SO    = 19;  // shared thermocouple data (MISO)
static const uint8_t PIN_MAX6675_CS_BT = 21;  // bean temperature (BT) chip-select
static const uint8_t PIN_MAX6675_CS_ET = 22;  // air/exhaust temperature (ET) chip-select
static const uint8_t PIN_RELAY_BURNER  = 25;  // burner relay (active-LOW)
static const uint8_t PIN_RELAY_DRUM    = 26;  // drum motor relay (active-LOW)
static const uint8_t PIN_BOOT_BUTTON   = 0;   // BOOT button — WiFi credential reset
