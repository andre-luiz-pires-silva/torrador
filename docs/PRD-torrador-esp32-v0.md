# PRD — ESP32 Coffee Roaster Controller

**Version:** V0 (Proof of Concept)
**Status:** Definitions closed, ready for implementation
**Last updated:** July 2026

---

## 0. Language & Branding Policy

### 0.1 Language
- **Code and system documentation: English is the default language.** All identifiers, code comments, commit messages, and technical documentation (including this PRD) are written in English.
- **User-facing text** (admin/operation web interface, operator-facing messages): **Portuguese (BR) only in V0**. The final plan is to support **both Portuguese and English** via internationalization (i18n). User-facing strings must therefore be structured to allow later localization (centralized strings, not hardcoded deep in logic).

### 0.2 Branding (white-label)
The firmware is **white-label**: the same codebase is sold to multiple manufacturers, each with its own identity. The product name (and, later, logo/colors) must come from a single **branding configuration**, never hardcoded across the code.

- **Compile-time branding**, one build per manufacturer (`branding.h` / build flags), not a runtime setting — zero runtime/RAM cost. Change the branding config, rebuild, and every user-visible surface follows. Default brand of this repo: **Torrador**.
- **V0 scope (where possible and not costly):** at minimum the product name, plus everything derived from it — web UI title/header, serial boot banner, mDNS default hostname (`torrador`), and AP SSID (`Torrador-Setup`). Deeper visual theming (logos, colors) may be layered on in later phases; the invariant is that no brand string is hardcoded deep in logic.
- Branding shares the same "centralized strings" pattern as i18n: the product name is a single value that user-facing strings interpolate.
- The **web UI** (static files on LittleFS) must read the brand from the firmware (e.g. the `/status` JSON or a small branding endpoint/generated file), not bake the product name into HTML/JS.

---

## 1. Overview

### 1.1 Product goal
Develop an electronic controller for a gas coffee roaster, based on ESP32, capable of:
1. Monitoring process temperatures (bean mass and air) in real time
2. Driving the burner and drum motor manually or automatically (simple min/max temperature control)
3. Providing a local web interface for operation and configuration
4. (Future phases) Integrating with the Artisan software via MODBUS TCP

### 1.2 V0 scope
V0 is a **proof of concept** to validate the integration of the ESP32 with the roaster. It **does not include** Artisan integration (Phase 3). It focuses on: real sensors, real relays (no load connected), web interface, min/max temperature control, and AP-based network provisioning.

### 1.2.1 Platform
- **Target:** ESP32 (WROOM), single board. Built with PlatformIO (one `esp32` environment; async transport `AsyncTCP`).
- **Code structure:** a single cooperative loop (no explicit FreeRTOS tasks); web pages served from LittleFS, not from RAM strings; all pins and board includes centralized in `board_config.h`.

### 1.3 Phase roadmap

| Phase | Goal | Hardware |
|---|---|---|
| **Phase 1 (V0, current)** | Validate control logic, temperature control, and web interface on a protoboard | ESP32 + 2x MAX6675 (real probes) + 2x relay module **with no load connected** |
| **Phase 2** | Connect real loads to the already-validated relays | Solenoid valve (burner) + drum motor |
| **Phase 3** | Artisan integration | MODBUS TCP server on top of the validated base |

---

## 2. Product Definitions

### 2.1 V0 features

#### F1 — Real-time temperature monitoring
- Continuous display of two temperatures in the web interface:
  - **BT** (Bean Temperature): temperature of the bean mass
  - **ET** (Environmental Temperature): air temperature
- Real-time updates (WebSocket or polling — see section 3.6)

#### F2 — Operation control (START/STOP)
- A single **START/STOP** control toggles the process on/off (the operator's run/stop).
- On STOP — and at boot — the burner is off. A latched fault (**LOCKOUT**) is cleared with the BOOT button.

#### F3 — Manual mode
- The controller runs in **manual** mode — it operates on its own settings. (The other mode, **Artisan** / MODBUS TCP slave, is Phase 3.)
- While the process is running, the manual-mode behaviour depends on configuration:
  - **min/max set** → the ESP regulates the burner to keep the temperature within the band (F4).
  - **min/max not set** → the flame follows START/STOP directly (on while running, off when stopped).

#### F4 — Temperature band (min/max)
> The gas-burner behaviour behind this feature (ignition, flame supervision, and the min/max regulation) is specified in `docs/design-flame-control.md`.
- Simple **min/max temperature band on BT**:
  - Below **min** → demand heat (burner on).
  - Above **max** → stop (burner off), with hysteresis between the two thresholds.
- **Off always wins:** reaching max, a flame fault, STOP, or a sensor fault always turns the burner off.
- The drum relay is a plain on/off output (no temperature logic).

#### F5 — Configuration persistence
- The configured min/max temperatures are saved to **LittleFS** and survive ESP restart/power-off
- On startup, the firmware automatically loads the saved settings

#### F6 — Network provisioning (AP configuration mode)
"IP camera" pattern: with no display or keyboard, network configuration is done by connecting to a Wi-Fi network created by the device itself. **Custom implementation** (not WiFiManager) — reuses the ESPAsyncWebServer and LittleFS already present in the project and keeps a visual identity consistent with the operation interface.

**Startup state machine:**
```
POWER ON
  |- No saved credentials -> CONFIGURATION MODE (AP)
  |- Saved credentials    -> OPERATION MODE (STA)
       |- Connected -> normal services (web, mDNS, control)
       |- Failed after timeout (e.g., 60s) -> CONFIGURATION MODE (automatic fallback)
```

**Configuration mode (AP):**
- The ESP creates its own network `{Brand}-Setup` (`Torrador-Setup` for the default brand, per §0.2); default IP `192.168.4.1`
- **Captive portal** via `DNSServer` answering all DNS queries with the ESP's IP — the phone opens the configuration page automatically on connect
- Configuration page served from LittleFS, containing:
  - SSID (with a "scan networks" button via `WiFi.scanNetworks()`)
  - Network password
  - mDNS name (optional; default `torrador`)
- On save: credentials persisted to LittleFS (`config.json`) -> restart in operation mode

**Configuration reset (3 complementary mechanisms):**
1. **Physical button at boot:** hold BOOT button (GPIO0) for X seconds at power-on -> erase credentials -> enter AP mode
2. **Automatic fallback:** connection failure after timeout -> AP mode (covers router/password change)
3. **Web interface button:** "Reset network configuration" option in operation mode

### 2.2 Web interface — screen/section structure

| Section | Elements |
|---|---|
| **Dashboard** | Real-time BT and ET reading; current burner state (idle/heating/hold/lockout); process running indicator |
| **Control** | START/STOP the process; reset a lockout |
| **Temperature** | Set the min and max temperatures (°C) for the burner band; leave blank to keep the flame following START/STOP |

*(Note: user-facing labels are in Portuguese (BR) for V0 per the Language Policy; the table above describes function in English.)*

### 2.3 Access requirements
- Access via browser on a computer or phone **on the same Wi-Fi network**
- Friendly address: `http://torrador.local` (mDNS), plus the direct IP
- **Plain HTTP, no HTTPS** — conscious decision: negligible risk on a local network; Chrome warnings ("Always Use Secure Connections", from Oct/2026) do not apply to local/private-network IPs
- **HTTP Basic Auth** (optional, to be assessed at implementation): a minimal layer to prevent undue access from other devices on the network

---

## 3. Technical Specifications

### 3.1 Hardware (Phase 1 / V0)

| Component | Qty | Function | Notes |
|---|---|---|---|
| ESP32 (WROOM) | 1 | Central controller | Dual-core and ~520KB RAM for the async web server + control logic simultaneously (and MODBUS in Phase 3) — see section 1.2.1 |
| MAX6675 (breakout) | 2 | Type-K thermocouple -> digital converter | SPI; range 0–1024°C; resolution 0.25°C; ~1 read every 220ms. Conscious decision to keep MAX6675 (simplicity) instead of MAX31855 (which would add thermocouple fault detection) — revisit in the future |
| Type-K thermocouple probe | 2 | BT: long/immersion probe in the mass; ET: short probe/air | Verify probe stem length compatible with the drum before purchasing |
| Relay module | 2 | Relay #1: burner; Relay #2: drum motor | In V0, **no load connected to the contacts** — the module's own indicator LED serves as visual feedback |
| Protoboard + jumpers | 1 | Prototype assembly | — |

### 3.2 Connections (logical definition)

- **MAX6675 (x2):** shared SPI — same SCK and SO for both chips; **individual CS** per chip (2 CS pins)
- **Relays (x2):** 1 digital GPIO per relay (HIGH/LOW). Note: common relay modules are active-LOW — confirm polarity
- Specific ESP pins: to be defined at implementation (avoid boot/strapping pins)

### 3.3 Firmware architecture

**Main modules:**

1. **Sensor reading:** periodic read cycle of the two MAX6675 via SPI (respecting the chip's ~220ms minimum interval)
2. **Temperature control:** min/max hysteresis on BT per read cycle:
   ```
   If min/max not both set -> demand heat (flame direct)
   Else:  BT <= min -> demand heat;  BT >= max -> stop;  in between -> keep state
   Off always wins (max reached, flame fault, STOP, sensor fault -> burner off)
   ```
3. **Web server:** ESPAsyncWebServer + async TCP (async — mandatory so as not to block the other tasks)
4. **File system:** LittleFS for (a) interface static files (HTML/CSS/JS) and (b) settings persistence (min/max)
5. **mDNS:** ESPmDNS library, registering `torrador.local`
6. **Network provisioning:** AP + captive portal state machine (see F6)

**Configuration (logical model):** grouped by operating mode (see `docs/design-flame-control.md` §7)
```
manual.temperature = {
  min_c: number | null,   // null = not configured
  max_c: number | null    // min_c < max_c
}
// Either unset => burner stays on directly (flame direct)
```

**Global system state:**
```
{
  mode: "manual",          // "manual" now; "artisan" (MODBUS slave) is Phase 3
  process: bool,           // START/STOP
  burnerState: "idle" | "run" | "hold" | "lockout" | "fault",
  temperature: { min_c, max_c },   // manual-mode config (see above)
  bt: float,               // last reading (°C)
  et: float                // last reading (°C)
}
```

### 3.4 HTTP API (routes)

**Operation mode:**

| Route | Method | Function |
|---|---|---|
| `/` | GET | Main page (dashboard, served from LittleFS) |
| `/status` | GET | JSON: BT, ET, burner state, process on/off |
| `/process` | POST | START/STOP the process (run/stop) |
| `/config/temperature` | GET | Return the current min/max (JSON) |
| `/config/temperature` | POST | Save min/max (or clear) and persist to LittleFS |
| `/network/reset` | POST | Erase network credentials and restart in configuration mode (AP) |

**Configuration mode (AP):**

| Route | Method | Function |
|---|---|---|
| `/` (and captive portal on any URL) | GET | Network configuration page |
| `/scan` | GET | JSON of visible Wi-Fi networks (`WiFi.scanNetworks()`) |
| `/save` | POST | Save SSID/password/mDNS name to `config.json` (LittleFS) and restart |

*(Route structure is a proposal — may be adjusted at implementation, keeping the semantics.)*

### 3.5 Network
- **HTTP** on port 80, no TLS
- **mDNS:** `torrador.local` (name configurable during provisioning; default derives from the branding config — `torrador` for the default brand, per §0.2)
- **Provisioning:** AP mode + captive portal (see F6); credentials in `config.json` on LittleFS
- **Reserved IP** on the router via DHCP reservation (ESP MAC) — user configuration, not firmware
- mDNS works only on the same local network (does not cross routers); native support on macOS/iOS/Android/Linux; on Windows it may depend on the Bonjour service

### 3.6 Open decision (to resolve at implementation)
- **Real-time dashboard updates:** WebSocket (`AsyncWebSocket`, push, more efficient) vs. polling via `fetch()` on `/status` (simpler). Both viable on the ESP; decide at the start of implementation.

---

## 4. Future Phases Overview (context for V0 decisions)

*The definitions below have already been discussed and should be considered in the V0 design to avoid rework, but they are **not part of the V0 scope**.*

### 4.1 Phase 2 — Real loads
- **Burner:** **single, on/off** solenoid valve (not proportional). Controlled via **time-proportioning** (time modulation, e.g., 10s cycle) once integrated with Artisan — in V0, control is on/off via rules only
- **Pending:** solenoid coil voltage (12V/24V/110V/220V) -> defines the final relay/SSR and any driver stage (transistor/optocoupler)
- **Drum motor:** pending decision on variable speed (AC+VFD or DC+PWM) vs. fixed (on/off). In V0 it is treated as simple on/off
- **Ventilation:** planned for future phases (PWM on a DC motor), out of V0 scope

### 4.2 Phase 3 — Artisan integration (MODBUS TCP)
- ESP = MODBUS TCP server/slave; Artisan = client/master
- Register map already defined:

| Register | Type | Address | Content | Status |
|---|---|---|---|---|
| Input Register | Read | 0 | BT (°C × 10) | Planned |
| Input Register | Read | 1 | ET (°C × 10) | Planned |
| Holding Register | Write | 0 | Burner power 0–100 (time-proportioning) | Planned |
| Holding Register | Write | 1 | Ventilation power 0–100 (PWM) | Planned |
| Holding Register | Write | 2 | Drum speed 0–100 | Reserved |
| Coil | Write | 0 | Drum on/off (if motor is fixed-speed) | Reserved |

- Temperature values transmitted as integer ×10 (MODBUS uses 16-bit registers, no decimals); Artisan configured with the corresponding divisor

---

## 5. Registered Pending Items

| # | Pending item | Blocks V0? | Depends on |
|---|---|---|---|
| 1 | Burner solenoid coil voltage | No | Physical hardware in hand (Phase 2) |
| 2 | Drum motor relay coil/load voltage | No | Physical hardware in hand (Phase 2) |
| 3 | Drum motor: variable or fixed speed | No | Equipment definition (Phase 2/3) |
| 4 | WebSocket vs. polling on the dashboard | No (decide at start of implementation) | Implementation preference |
| 5 | HTTP Basic Auth: include in V0 or not | No | Implementation preference |

---

## 6. V0 Acceptance Criteria

1. Web interface reachable at `http://torrador.local` from a computer and phone on the same network
2. Dashboard displays BT and ET in real time, with plausible values from the real probes
3. In manual mode, START/STOP runs and stops the process (burner off on STOP and at boot; LOCKOUT cleared with BOOT)
4. With min/max set, the burner regulates to the band on BT (off always wins)
5. The min and max temperatures can be set, cleared, and saved
6. The min/max settings persist after restarting the ESP
7. With min/max unset, the flame follows START/STOP (on while running, off when stopped)
8. With no saved credentials, the device comes up in AP mode (`Torrador-Setup`) and the captive portal opens the configuration page automatically when connecting from a phone
9. After saving SSID/password in the portal, the device restarts and connects to the configured network
10. Connection failure after timeout automatically returns to AP mode; the physical boot button and a web interface option also reset the network configuration
11. User-facing interface text is in Portuguese (BR), with strings structured to allow future English localization
12. The product name is not hardcoded: it comes from the branding config, and changing it (plus rebuilding) updates the web UI, serial boot banner, mDNS default hostname, and AP SSID (white-label, per §0.2)
