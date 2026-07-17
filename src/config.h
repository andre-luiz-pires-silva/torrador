#pragma once

#include <Arduino.h>

#include "branding.h"   // BRAND_MDNS_HOST — default mDNS hostname

// -----------------------------------------------------------------------------
// config.h — central runtime configuration, persisted to LittleFS (/config.json).
//
// Settings that belong to a specific operating mode live under that mode's group:
//   * automatic — the ESP regulates BT to a min/max band (JSON key "auto")
//   * artisan   — MODBUS TCP slave, driven by the Artisan software (future)
// Manual mode has no settings (the burner follows START/STOP directly).
//
// To add a setting: add a field to the right group struct (or a new group),
// then extend the (de)serialization in config.cpp. Unset temperatures use NAN.
// -----------------------------------------------------------------------------

// --- Active control authority (who owns the heat demand) ---
//   MANUAL  — the operator drives START/STOP directly (default)
//   AUTO    — the ESP's own min/max regulator owns the demand
//   ARTISAN — the Artisan software owns the demand via MODBUS (Phase 2)
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

// --- Automatic mode: the ESP regulates BT to a min/max band ---
struct AutoConfig {
  TemperatureConfig temperature;
};

// --- Safety: independent over-temperature cutoff (applies in EVERY mode) ---
// A hard ceiling on BT, independent of the AUTO regulation band. If BT reaches
// it in any mode (manual/auto/artisan), the burner is cut regardless of what the
// operator or Artisan is asking — "off always wins". NaN = not configured.
struct SafetyConfig {
  float hardMaxC = NAN;   // NaN = disabled
  bool configured() const { return !isnan(hardMaxC); }
};

// --- Artisan mode: MODBUS TCP slave to the Artisan software (Phase 2) ---
// Artisan is the MODBUS client/master: it reads BT/ET and writes a burner-power
// register (0..100). The ESP maps that power to an on/off burner demand by a
// threshold (power > threshold => demand heat) — proportional modulation is
// deferred to Phase 3 (PRD §4.1). Temperatures are exchanged as °C×10.
struct ArtisanConfig {
  uint16_t port           = 502;   // MODBUS TCP listen port (502 = IANA default)
  uint8_t  powerThreshold = 50;    // burner ON when Artisan power (0..100) exceeds this
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
  // Optional HTTP Basic credentials gating the whole web UI. The password being
  // empty means auth is disabled: the interface is open (acceptable on a trusted
  // network). When set, it guards the UI, /status, config and burner commands.
  // The username is shown/editable in the UI (default BRAND_ADMIN_USER) so the
  // operator knows what to type at the browser login prompt.
  char    adminUser[33] = BRAND_ADMIN_USER; // web-UI login username (never empty)
  char    adminPassword[65] = "";           // "" = auth disabled
  bool    staProvisioned() const { return ssid[0] != '\0'; }
  bool    adminAuthEnabled() const { return adminPassword[0] != '\0'; }
};

// --- Root configuration ---
struct Config {
  Mode          mode = Mode::MANUAL;   // active control authority (default MANUAL)
  AutoConfig    automatic;             // AUTO-mode band ("auto" is a reserved word)
  ArtisanConfig artisan;
  SafetyConfig  safety;                // independent over-temp cutoff (all modes)
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
