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
2. Driving the burner and drum motor manually or automatically (configurable rule engine)
3. Providing a local web interface for operation and configuration
4. (Future phases) Integrating with the Artisan software via MODBUS TCP

### 1.2 V0 scope
V0 is a **proof of concept** to validate the integration of the ESP32 with the roaster. It **does not include** Artisan integration (Phase 3). It focuses on: real sensors, real relays (no load connected), web interface, rule engine, and AP-based network provisioning.

### 1.2.1 Platform strategy (ESP8266 -> ESP32)
- **Final target:** ESP32 (WROOM)
- **Initial development:** ESP8266 (hardware already available), with a compatibility layer for the whole ESP family
- **Build tool:** PlatformIO with two environments (`esp8266` and `esp32`), resolving environment-specific dependencies (ESPAsyncTCP vs. AsyncTCP)
- **Portability rules:**
  1. No ESP32-exclusive APIs (explicit FreeRTOS tasks, pinned cores) — single cooperative loop
  2. Conserve RAM as if always on ESP8266 (pages served from LittleFS, not from RAM strings)
  3. Board-specific pins and WiFi/mDNS includes centralized in a single `board_config.h` with `#ifdef ESP32 / #else`
- Migrating to ESP32 must require only switching the build environment, with no logic changes

### 1.3 Phase roadmap

| Phase | Goal | Hardware |
|---|---|---|
| **Phase 1 (V0, current)** | Validate control logic, rule engine, and web interface on a protoboard | ESP32 + 2x MAX6675 (real probes) + 2x relay module **with no load connected** |
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

#### F2 — Manual relay control
- Independent on/off button for each relay:
  - Relay #1: burner
  - Relay #2: drum motor
- Available when the system is in manual mode

#### F3 — Manual / automatic mode
- **Single toggle** that switches the whole system between manual and automatic
- The toggle affects **both relays simultaneously** (no mixed mode)
- In automatic mode, manual buttons are disabled and the rule engine takes control

#### F4 — Configurable rule engine (automatic mode)
- **Two independent instances**: one for the burner relay, one for the drum relay
- Each instance has two independent rules:
  - **Activation rule** (when to turn the relay on)
  - **Deactivation rule** (when to turn the relay off)
- Each rule is composed of:
  - A list of **conditions** (minimum 1, **maximum 5**)
  - A **logical operator** (AND or OR) combining all conditions of the rule
- Each condition contains:
  - **Sensor:** BT or ET
  - **Comparator:** `<` or `>`
  - **Value:** temperature in °C
- **Tie priority:** if the activation and deactivation rules are both satisfied in the same cycle, **deactivation always wins** (safety first)

#### F5 — Configuration persistence
- Configured rules are saved to **LittleFS** and survive ESP restart/power-off
- On startup, the firmware automatically loads the saved rules

#### F6 — Network provisioning (AP configuration mode)
"IP camera" pattern: with no display or keyboard, network configuration is done by connecting to a Wi-Fi network created by the device itself. **Custom implementation** (not WiFiManager) — reuses the ESPAsyncWebServer and LittleFS already present in the project and keeps a visual identity consistent with the operation interface.

**Startup state machine:**
```
POWER ON
  |- No saved credentials -> CONFIGURATION MODE (AP)
  |- Saved credentials    -> OPERATION MODE (STA)
       |- Connected -> normal services (web, mDNS, rules)
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
| **Dashboard** | Real-time BT and ET reading; current state of both relays (on/off); current mode indicator (manual/automatic) |
| **Manual control** | On/off button for the burner relay; on/off button for the drum relay (enabled only in manual mode) |
| **Mode** | Single manual/automatic toggle |
| **Rules — Burner** | Editor for the activation rule and the deactivation rule: list of conditions (sensor + comparator + value), AND/OR selector, "add condition" button (limit: 5) |
| **Rules — Drum** | Same structure as the burner editor, fully independent |

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
| ESP32 (WROOM) — final target | 1 | Central controller | Chosen over ESP8266 for dual-core and ~520KB RAM — needed for async web server + control logic simultaneously (and MODBUS in Phase 3). **Initial development on ESP8266** (available hardware) with a compatibility layer — see section 1.2.1 |
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
2. **Rule engine:** evaluation per read cycle, for each relay:
   ```
   1. Evaluate DEACTIVATION rule
      If satisfied -> turn relay off, end cycle (ignore activation rule)
   2. Otherwise, evaluate ACTIVATION rule
      If satisfied -> turn relay on
   3. If none satisfied -> keep current relay state
   ```
3. **Web server:** ESPAsyncWebServer + async TCP (async — mandatory so as not to block the other tasks)
4. **File system:** LittleFS for (a) interface static files (HTML/CSS/JS) and (b) rule persistence
5. **mDNS:** ESPmDNS library, registering `torrador.local`
6. **Network provisioning:** AP + captive portal state machine (see F6)

**Rule data structure (logical model):**
```
RelayConfig = {
  activation_rule: Rule,
  deactivation_rule: Rule
}

Rule = {
  operator: "AND" | "OR",
  conditions: [           // 1 to 5 items
    {
      sensor: "BT" | "ET",
      comparator: "<" | ">",
      value: number (°C)
    }
  ]
}

// Two instances: burnerRelay and drumRelay, fully independent
```

**Global system state:**
```
{
  mode: "manual" | "automatic",   // single toggle, affects both relays
  burnerRelay: { state: bool, config: RelayConfig },
  drumRelay:   { state: bool, config: RelayConfig },
  bt: float,   // last reading (°C)
  et: float    // last reading (°C)
}
```

### 3.4 HTTP API (routes)

**Operation mode:**

| Route | Method | Function |
|---|---|---|
| `/` | GET | Main page (dashboard, served from LittleFS) |
| `/status` | GET | JSON: BT, ET, relay states, current mode |
| `/relay/{burner\|drum}/on` | POST | Turn relay on manually (manual mode only) |
| `/relay/{burner\|drum}/off` | POST | Turn relay off manually (manual mode only) |
| `/mode` | POST | Toggle manual/automatic (single toggle) |
| `/rules/{burner\|drum}` | GET | Return the relay's current rules (JSON) |
| `/rules/{burner\|drum}` | POST | Save rules (activation + deactivation) and persist to LittleFS |
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
3. In manual mode, each relay turns on/off via its corresponding button (feedback visible on the module LED)
4. In automatic mode, each relay responds to its configured rules, with deactivation prevailing on ties
5. Rules with up to 5 conditions and an AND/OR operator can be created, edited, and saved per relay
6. Rules persist after restarting the ESP
7. The single toggle switches manual/automatic for both relays simultaneously
8. With no saved credentials, the device comes up in AP mode (`Torrador-Setup`) and the captive portal opens the configuration page automatically when connecting from a phone
9. After saving SSID/password in the portal, the device restarts and connects to the configured network
10. Connection failure after timeout automatically returns to AP mode; the physical boot button and a web interface option also reset the network configuration
11. The firmware compiles and runs on both ESP8266 and ESP32 from the same code, changing only the build environment (PlatformIO)
12. User-facing interface text is in Portuguese (BR), with strings structured to allow future English localization
13. The product name is not hardcoded: it comes from the branding config, and changing it (plus rebuilding) updates the web UI, serial boot banner, mDNS default hostname, and AP SSID (white-label, per §0.2)
