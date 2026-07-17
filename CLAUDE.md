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

Firmware for a gas coffee roaster controller. It monitors bean temperature (BT) and air temperature (ET) via 2x MAX6675 (type-K thermocouple, SPI), commands a dedicated gas flame controller (**Inova INV-27109** — which drives the solenoid valve, spark, and ionization flame sensing), and serves a local web interface for operation and configuration. The ESP enables the flame controller and supervises it via its fault output. Current phase: **V0 — proof of concept** (real actuators with no gas/load connected).

**Roadmap (reordered 2026-07):** Phase 1 (V0, bench dry) → **Phase 2 — Artisan integration** (MODBUS TCP, still dry, on the validated base) → **Phase 3 — real system / gas** (real INV + gas valve, the final step). Artisan integration was brought forward ahead of the real gas assembly because it is pure firmware/network work needing no gas; assembling real combustion last de-risks the project. Do not implement Artisan/MODBUS until Phase 2 is started.

**Full requirements, features, and acceptance criteria:** `docs/PRD-torrador-esp32-v0.md`. Consult before implementing any feature.

**Gas burner control (ignition, flame supervision, min/max hold):** `docs/design-flame-control.md` — the design & ADR for the current development phase. Consult before touching burner/flame/ignition code.

**Hardware wiring (connection tables, BOM, isolation boundary):** `docs/hardware/wiring.md`. `board_config.h` is the pin authority; keep the wiring doc in sync.

## Language policy

- **Code and system documentation: English is the default language.** All identifiers, code comments, commit messages, technical docs (PRD, this file, ADRs), and internal artifacts are written in English.
- **User-facing text** (admin/operation web interface, messages shown to the operator): **Portuguese (BR) only in V0**. The final plan is to support **both Portuguese and English** (i18n), so user-facing strings must be structured to allow localization later (avoid hardcoding UI strings deep in logic; keep them in a single place that can become a translation table).

## Branding / white-label

The firmware is **white-label**: the same codebase ships to multiple manufacturers, each with its own identity. The product name (and later logo/colors) MUST come from a single branding configuration — never hardcode a brand string across the codebase.

- **Branding is compile-time**, one build per manufacturer (a `branding.h` / build flags), not a runtime setting. Change the branding config, rebuild, and every user-visible surface follows. Default brand of this repo: **Torrador**.
- **Where possible and not costly** (V0 target): at minimum the product name string, plus everything derived from it — web UI title/header, serial boot banner, mDNS default hostname, and AP SSID. Deeper visual theming (logos, colors) can be layered on later; the invariant is *no brand string hardcoded deep in logic*.
- The branding config sets the **defaults** for identity strings: mDNS host (`torrador` for the default brand), AP SSID (`Torrador`). The mDNS name stays user-overridable during provisioning (F6).
- **Same pattern as i18n:** the product name is a single centralized value that user-facing strings interpolate (see Language policy). Keep branding and translatable strings in one place.
- **Web UI (static files on LittleFS):** the front-end must read the brand from the firmware (e.g. the `/status` JSON or a small branding endpoint/generated file), not bake the product name into HTML/JS.

## Platform and build

- **Target: ESP32 (WROOM).** Single board; no dual-board support.
- Build via **PlatformIO**, one environment: `esp32`. Async transport dependency is `AsyncTCP`.

## Code structure rules (non-negotiable)

1. **Single cooperative loop for V0**: no explicit FreeRTOS tasks. Keep the control loop simple and predictable; revisit only if a concrete need arises.
2. **Serve web pages from LittleFS, never from RAM strings**; use moderate JSON buffers; prefer fixed buffers over `String` where critical.
3. **All board/pin configuration lives in `board_config.h`**: WiFi/mDNS/async-transport includes and pin definitions. No pin number hardcoded outside this file.

## Technical stack (decided — do not replace)

- **ESPAsyncWebServer** (async server; never use the synchronous `WebServer`)
- **LittleFS** for: interface static files (HTML/CSS/JS) and persisted settings (`/config.json`, incl. network provisioning later). Runtime settings live in a `config` module (`src/config.h` / `src/config.cpp`), grouped by area
- **ESPmDNS**: default name derives from the branding config (`torrador` for the default brand) -> `http://torrador.local`
- **Plain HTTP, port 80, no TLS** — conscious decision (local network); do not "improve" to HTTPS
- WiFi provisioning: **custom implementation** (AP + captive portal with `DNSServer`) — do not use WiFiManager

## Critical domain rules (safety)

- Temperature regulation (in **automatic** mode) is a simple **min/max band on BT** (below min → demand heat; above max → stop, with hysteresis). Both limits are required for automatic mode. No rule engine, no BT/ET selection — deliberately minimal.
- An **independent over-temperature cutoff** (`hard_max_temp_c`, in `config.safety`) applies in **every** mode and latches a LOCKOUT when BT reaches it — a safety backstop above the regulation band, also protecting against a remote Artisan command. Optional (unset = disabled).
- **Turning the burner OFF always takes precedence** (max reached, over-temp cutoff, flame fault, STOP/e-stop, or sensor fault). This is a thermal-safety requirement, not an implementation detail.
- **Three control modes** (`config.mode`): **manual** — flame follows START/STOP directly (band ignored); **automatic** — the ESP regulates BT to the min/max band; **Artisan** — MODBUS TCP slave, Artisan owns the demand (Phase 2), and the front button becomes a latched emergency stop. Switching mode always cuts the burner first.
- MAX6675: minimum ~220ms interval between reads of the same chip — respect this in the read cycle.
- Shared SPI between the two MAX6675 (same SCK/SO), individual CS.
- Common relay modules are **active-LOW** — confirm the module's polarity before assuming HIGH=on.

**Gas burner / flame safety** (see `docs/design-flame-control.md`; controller manual `docs/manuals/Manual_INV_27109v9.1.pdf`):
- Combustion is delegated to a dedicated flame controller (**Inova INV-27109**): it drives the spark and gas valve, senses flame by ionization, and closes gas on flame loss. The ESP does NOT drive the valve/spark directly.
- The ESP **enables** the controller by power-gating its mains supply through a relay; **power off ⇒ gas closes** (fail-safe). Force the enable output OFF at the very start of `setup()`, before anything else.
- The ESP reads the controller's fault output (12V buzzer) through a **PC817 optocoupler** — galvanic isolation; never tie the 12V ground to the ESP ground.
- The INV-27109 does **not** latch, and its manual states it **must not be used alone as a safety system**. Therefore the ESP implements a **master LOCKOUT** (cut power, refuse to re-enable until a BOOT short press). An independent mechanical safety backstop on the gas line is strongly recommended at installation but is **out of scope of this firmware** (integrator's responsibility).
- Any fault/absent flame signal is treated as no flame; a **watchdog** cuts the enable (closes gas) if the loop stalls; `hard_max_temp_c` is an independent over-temperature cutoff.
- This phase runs **DRY**: no real gas; the fault/flame input is exercised with a push-button (or the real INV, which faults with no gas).

## Network provisioning (PRD F6)

- No saved credentials -> AP mode `Torrador` (IP `192.168.4.1`) + captive portal. AP SSID is `{Brand}`, derived from the branding config (`Torrador` for the default brand).
- STA connection failure after timeout (~60s) -> automatic fallback to AP mode.
- Credential reset: BOOT button held at boot, automatic fallback, and `/network/reset` route in the interface.

## Decisions already made (do not reopen unless the author asks)

| Decision | Rationale |
|---|---|
| MAX6675 (not MAX31855) | Simplicity in V0; no thermocouple fault detection for now; future upgrade is straightforward |
| Plain HTTP, no HTTPS | Local network; TLS costs RAM/CPU; Chrome warnings don't affect private IPs |
| Custom provisioning (not WiFiManager) | Reuses async stack + single visual identity |
| Three explicit control modes: manual / automatic / Artisan (not per relay) | Product decision; each mode owns the heat demand differently; mode switch cuts the burner first |
| Independent over-temperature cutoff (`hard_max_temp_c`) in all modes | Safety backstop above the band; also guards against a remote Artisan command |
| Roadmap reordered: Artisan (Phase 2) before real gas assembly (Phase 3) | Artisan is firmware-only, validated dry; assembling real combustion last de-risks the project |
| Burner-off always takes precedence (max/over-temp/fault/stop) | Thermal safety |
| Simple min/max temperature control (no rule engine) | Minimal for V0; flexible rules dropped; re-evaluate later if needed |
| ESP32-only (ESP8266 dropped) | Simpler build/code; dual-core + RAM for simultaneous web + MODBUS in Phase 2 |
| White-label branding, compile-time | One codebase, many manufacturers; per-build identity with no runtime cost |
| Gas burner: delegate combustion to Inova INV-27109; ESP enables + supervises (fault via PC817 opto); ESP-level lockout + mechanical backstop | Purpose-built controller does ignition/ionization/valve safely; simpler firmware; safer for a commercial product; see `docs/design-flame-control.md` |

## Open (decide during implementation, ask if relevant)

- ~~WebSocket vs. polling for dashboard updates~~ → resolved: **polling** `/status` every 1 s
- ~~HTTP Basic Auth: include in V0 or not~~ → resolved: **optional HTTP Basic** (`config.network.adminPassword`, empty = disabled) gating the **whole** UI, with a **configurable username** (`adminUser`, default `admin` from `BRAND_ADMIN_USER`). Set/cleared in the **Segurança** settings tab (which also holds `hard_max_temp_c` and an access disclaimer), or via the serial `auth` command (recovery path). See PRD F6a.
- Final pin assignment (avoid ESP32 boot/strapping pins)
- Artisan burner-power → actuation mapping beyond on/off threshold (revisit in Phase 3)

## Out of scope for Phase 1 / V0 (do not implement here)

- MODBUS TCP / Artisan integration (**Phase 2** — start only when that phase begins)
- Burner time-proportioning / flame modulation (**Phase 3**, measured against the real INV; in V0/Phase 2 the burner is on/off, and Artisan's burner-power maps to on/off by threshold)
- Real gas actuation / real INV assembly (**Phase 3**)
- **Drum-motor automation** (on/off or variable speed), **ventilation control**, and any other more advanced automation — deliberately out of the initial scope (V0 drives only the burner). See PRD §4.1.
