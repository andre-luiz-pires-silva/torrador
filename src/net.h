#pragma once

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
