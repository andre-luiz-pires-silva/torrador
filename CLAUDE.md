# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# Coffee Roaster Controller (ESP)

## Repository status

Scaffolded: `platformio.ini` (single `esp32` env), `src/` (skeleton firmware + `board_config.h`), and `data/` (LittleFS web UI) exist. **V0 — proof of concept** phase. Follow the stack and code-structure rules below; do not introduce a build system other than PlatformIO.

## Build & commands (PlatformIO)

Single environment (`esp32`); do not introduce a build system other than PlatformIO.

- Build: `pio run`
- Flash firmware + serial monitor: `pio run -t upload -t monitor`
- Build/upload the LittleFS image (web UI in `data/`, rules, `config.json`): `pio run -t buildfs` / `-t uploadfs` — remember firmware and filesystem are flashed separately.
- Serial monitor only: `pio device monitor`

## What this project is

Firmware for a gas coffee roaster controller. It monitors bean temperature (BT) and air temperature (ET) via 2x MAX6675 (type-K thermocouple, SPI), drives 2 relays (burner and drum motor), and serves a local web interface for operation and configuration. Current phase: **V0 — proof of concept** (real relays with no load connected). Integration with the Artisan software via MODBUS TCP comes in Phase 3 — do not implement yet.

**Full requirements, features, and acceptance criteria:** `docs/PRD-torrador-esp32-v0.md`. Consult before implementing any feature.

## Language policy

- **Code and system documentation: English is the default language.** All identifiers, code comments, commit messages, technical docs (PRD, this file, ADRs), and internal artifacts are written in English.
- **User-facing text** (admin/operation web interface, messages shown to the operator): **Portuguese (BR) only in V0**. The final plan is to support **both Portuguese and English** (i18n), so user-facing strings must be structured to allow localization later (avoid hardcoding UI strings deep in logic; keep them in a single place that can become a translation table).

## Platform and build

- **Target: ESP32 (WROOM).** Single board; no dual-board support.
- Build via **PlatformIO**, one environment: `esp32`. Async transport dependency is `AsyncTCP`.

## Code structure rules (non-negotiable)

1. **Single cooperative loop for V0**: no explicit FreeRTOS tasks. Keep the control loop simple and predictable; revisit only if a concrete need arises.
2. **Serve web pages from LittleFS, never from RAM strings**; use moderate JSON buffers; prefer fixed buffers over `String` where critical.
3. **All board/pin configuration lives in `board_config.h`**: WiFi/mDNS/async-transport includes and pin definitions. No pin number hardcoded outside this file.

## Technical stack (decided — do not replace)

- **ESPAsyncWebServer** (async server; never use the synchronous `WebServer`)
- **LittleFS** for: interface static files (HTML/CSS/JS), persisted rules, network `config.json`
- **ESPmDNS**: default name `torrador` -> `http://torrador.local`
- **Plain HTTP, port 80, no TLS** — conscious decision (local network); do not "improve" to HTTPS
- WiFi provisioning: **custom implementation** (AP + captive portal with `DNSServer`) — do not use WiFiManager

## Critical domain rules (safety)

- Rule engine: 2 independent instances (burner and drum), each with an activation rule and a deactivation rule (up to 5 conditions each, AND/OR operator, conditions over BT/ET with `<`/`>`).
- **Evaluation order per cycle: deactivation is ALWAYS evaluated first and ALWAYS wins ties.** This is a thermal-safety requirement, not an implementation detail.
- The manual/automatic toggle is **single** and affects both relays simultaneously. In automatic mode, manual relay commands are rejected.
- MAX6675: minimum ~220ms interval between reads of the same chip — respect this in the read cycle.
- Shared SPI between the two MAX6675 (same SCK/SO), individual CS.
- Common relay modules are **active-LOW** — confirm the module's polarity before assuming HIGH=on.

## Network provisioning (PRD F6)

- No saved credentials -> AP mode `Torrador-Setup` (IP `192.168.4.1`) + captive portal.
- STA connection failure after timeout (~60s) -> automatic fallback to AP mode.
- Credential reset: BOOT button held at boot, automatic fallback, and `/network/reset` route in the interface.

## Decisions already made (do not reopen unless the author asks)

| Decision | Rationale |
|---|---|
| MAX6675 (not MAX31855) | Simplicity in V0; no thermocouple fault detection for now; future upgrade is straightforward |
| Plain HTTP, no HTTPS | Local network; TLS costs RAM/CPU; Chrome warnings don't affect private IPs |
| Custom provisioning (not WiFiManager) | Reuses async stack + single visual identity |
| Single manual/auto toggle (not per relay) | Product decision |
| Deactivation priority on ties | Thermal safety |
| ESP32-only (ESP8266 dropped) | Simpler build/code; dual-core + RAM for simultaneous web + MODBUS in Phase 3 |

## Open (decide during implementation, ask if relevant)

- WebSocket vs. polling for dashboard updates
- HTTP Basic Auth: include in V0 or not
- Final pin assignment (avoid ESP32 boot/strapping pins)

## Out of scope for V0 (do not implement)

- MODBUS TCP / Artisan integration (Phase 3)
- Burner time-proportioning (Phase 3; in V0 control is on/off via rules)
- Ventilation control
- Variable drum motor speed
