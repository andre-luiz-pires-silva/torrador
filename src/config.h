#pragma once

#include <Arduino.h>

#include "branding.h"   // BRAND_MDNS_HOST — default mDNS hostname

// -----------------------------------------------------------------------------
// config.h — central runtime configuration, persisted to LittleFS (/config.json).
//
// Organized by operating mode, then by area. The controller has two modes:
//   * manual  — runs on its own settings (today: min/max temperature control)
//   * artisan — MODBUS TCP slave, driven by the Artisan software (future)
//
// To add a setting: add a field to the right group struct (or a new group),
// then extend the (de)serialization in config.cpp. Unset temperatures use NAN.
// -----------------------------------------------------------------------------

// --- Active control authority (who owns the heat demand) ---
//   MANUAL  — the operator drives START/STOP directly (default)
//   AUTO    — the ESP's own min/max regulator owns the demand
//   ARTISAN — the Artisan software owns the demand via MODBUS (Phase 3)
// In AUTO and ARTISAN the front push button acts as an emergency stop.
enum class Mode : uint8_t { MANUAL, AUTO, ARTISAN };

// Mode <-> its lowercase name ("manual" / "auto" / "artisan").
const char *modeName(Mode m);
bool parseMode(const char *s, Mode &out);

// --- Temperature regulation (min/max band on BT) ---
struct TemperatureConfig {
  float minC = NAN;   // NaN = not configured
  float maxC = NAN;   // NaN = not configured
  bool configured() const { return !isnan(minC) && !isnan(maxC); }
};

// --- Manual mode: the controller runs on its own configuration ---
struct ManualConfig {
  TemperatureConfig temperature;
};

// --- Artisan mode: MODBUS TCP slave to the Artisan software (future settings) ---
struct ArtisanConfig {
  // e.g. MODBUS port, register options — to be defined in Phase 3.
};

// --- Network / Wi-Fi (PRD F6) ---
// Two network modes:
//   AP  — the device IS the access point (WPA2), permanently. Default: usable
//         out of the box with no site Wi-Fi. Also the config/fallback surface.
//   STA — join an existing Wi-Fi; on failure the firmware falls back to AP.
// Fixed buffers (no String) per the code-structure rules. Passwords are stored
// in plaintext — acceptable in V0 (local network, documented).
enum class NetMode : uint8_t { AP, STA };

// NetMode <-> its lowercase name ("ap" / "sta"). Distinct from control Mode.
const char *netModeName(NetMode m);
bool parseNetMode(const char *s, NetMode &out);

struct NetworkConfig {
  NetMode mode        = NetMode::AP;        // AP = permanent access point (default); STA = join a network
  char    ssid[33]    = "";                 // STA SSID (max 32 + NUL); "" = no STA target
  char    password[65]= "";                 // STA WPA2 passphrase (max 63 + NUL)
  char    apPassword[65] = BRAND_AP_PASSWORD;// AP WPA2 passphrase (always required, >=8)
  char    mdnsHost[33]= BRAND_MDNS_HOST;    // user-overridable; defaults to brand
  bool    staProvisioned() const { return ssid[0] != '\0'; }
};

// --- Root configuration ---
struct Config {
  Mode          mode = Mode::MANUAL;   // active control authority (default MANUAL)
  ManualConfig  manual;
  ArtisanConfig artisan;
  NetworkConfig network;
};

// Global instance.
extern Config config;

// Mount LittleFS and load /config.json into `config` (defaults if absent/invalid).
void configBegin();

// Persist `config` to /config.json. Returns true on success.
bool configSave();

// Restore defaults in memory (call configSave() to also persist).
void configReset();
