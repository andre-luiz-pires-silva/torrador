#pragma once

#include <Arduino.h>   // uint8_t, NAN — keep this header self-contained

// -----------------------------------------------------------------------------
// net.h — Wi-Fi / web-server bring-up (PRD F6).
//
// Two network modes (config.network.mode):
//   AP  — the device IS the WPA2 access point, permanently. Default; also the
//         config surface and the STA-failure fallback. Captive portal + mDNS.
//   STA — join an existing Wi-Fi; on connect failure (~60 s) fall back to AP.
//
// All Wi-Fi / async / DNS includes live in board_config.h (code-structure rule).
// -----------------------------------------------------------------------------

// Bring up networking per config.network.mode: connect STA (with AP fallback) or
// start the permanent AP. Registers web routes and mDNS. Call after configBegin()
// and after the INV_ENABLE fail-safe in setup().
void netBegin();

// Service the captive-portal DNS while in AP mode (non-blocking) and perform the
// deferred reboot after a successful /save. Call every loop() iteration; the
// async web server itself needs no pump.
void netLoop();

// --- Home dashboard bridge (control loop <-> web) ----------------------------
// The web server runs on the AsyncTCP task; the control loop on loop(). These
// two calls pass live status out and user commands in. Kept intentionally small
// (scalar fields, single-slot command) — good enough for V0.

// Live status the `/status` endpoint serves. Published by the control loop.
struct AppStatus {
  char  state[12] = "IDLE";   // state code: IDLE/RUN/HOLD/LOCKOUT/ESTOP/FAULT
  float btC       = NAN;      // bean temperature (BT); NaN on sensor fault (drives control)
  float etC       = NAN;      // air/exhaust temperature (ET); NaN on sensor fault (telemetry)
  bool  processOn = false;    // process START/STOP latch
  // Artisan-mode telemetry (meaningful only in ARTISAN mode; the UI shows it there).
  bool    artisanLinked = false;  // Artisan (MODBUS TCP client) currently connected
  uint8_t artisanPower  = 0;      // last burner power commanded by Artisan (0..100)
};
void netPublishStatus(const AppStatus &s);

// Web-issued control commands, consumed once by the control loop.
//   START_STOP — same effect as the physical START/STOP button (toggles the
//                process, or releases a latched LOCKOUT/ESTOP when latched)
enum class NetCommand : uint8_t { NONE, START_STOP };
NetCommand netTakeCommand();

// Active interface, for the display: "AP" or "STA". Reflects reality — a STA
// connect that failed has fallen back to AP.
const char *netActiveMode();
