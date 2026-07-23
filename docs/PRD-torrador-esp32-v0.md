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
- **V0 scope (where possible and not costly):** at minimum the product name, plus everything derived from it — web UI title/header, serial boot banner, mDNS default hostname (`torrador`), and AP SSID (`Torrador`). Deeper visual theming (logos, colors) may be layered on in later phases; the invariant is that no brand string is hardcoded deep in logic.
- Branding shares the same "centralized strings" pattern as i18n: the product name is a single value that user-facing strings interpolate.
- The **web UI** (static files on LittleFS) must read the brand from the firmware (e.g. the `/status` JSON or a small branding endpoint/generated file), not bake the product name into HTML/JS.

---

## 1. Overview

### 1.1 Product goal
Develop an electronic controller for a gas coffee roaster, based on ESP32, capable of:
1. Monitoring process temperatures (bean mass and air) in real time
2. Driving the burner manually or automatically (simple min/max temperature control)
3. Providing a local web interface for operation and configuration
4. (Future phases) Integrating with the Artisan software via MODBUS TCP

### 1.2 V0 scope
V0 is a **proof of concept** to validate the integration of the ESP32 with the roaster. It focuses on: real sensors, real relays (no load connected), web interface, three control modes (manual / automatic / Artisan), min/max temperature control, an independent over-temperature cutoff, a loop watchdog, and AP-based network provisioning. The **Artisan integration itself** is developed in **Phase 2** (still dry, on top of this validated base); **real gas actuation** is deferred to the **final phase (Phase 3)**.

### 1.2.1 Platform
- **Target:** ESP32 (WROOM), single board. Built with PlatformIO (one `esp32` environment; async transport `AsyncTCP`).
- **Code structure:** a single cooperative loop (no explicit FreeRTOS tasks); web pages served from LittleFS, not from RAM strings; all pins and board includes centralized in `board_config.h`.

### 1.3 Phase roadmap

> **Ordering note (2026-07):** the roadmap was reordered to bring **Artisan
> integration forward (Phase 2)**, ahead of the **real gas-actuation assembly
> (Phase 3)**. Rationale: Artisan integration is pure firmware/network work on top
> of the already-validated bench base (it commands the same burner-enable output,
> dry, with the LED/relay + fault button) and needs no gas. Assembling the real
> INV-27109 + gas valve last de-risks the project — all control logic *and* the
> Artisan link are validated dry before any combustion.

| Phase | Goal | Hardware |
|---|---|---|
| **Phase 1 (V0, done)** | Validate control logic, the three control modes, min/max temperature control, the over-temperature cutoff + watchdog, the web interface, and network provisioning on a protoboard | ESP32 + 2x MAX6675 (real probes) + 1x relay module (burner enable) **with no load connected** |
| **Phase 2 — Artisan integration** | MODBUS TCP slave on top of the validated base, **still dry** (no gas) | Same bench hardware (burner enable = LED/relay, no load) |
| **Phase 3 — Real system / gas** | Assemble the real flame controller and gas path; real burner actuation | Real INV-27109 + 110 VAC solenoid valve (NC) + RC snubber + mains-rated enable relay + PC817 opto |

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
- On STOP — and at boot — the burner is off. A latched fault (**LOCKOUT**) is cleared with a short press of the same START/STOP button.

#### F3 — Control modes (manual / automatic / Artisan)
The controller has **three explicit control modes** (who owns the heat demand). The mode is selected in the web UI (and over serial); switching mode always cuts the burner first — control authority must not inherit a running flame.
- **Manual** — the operator drives the burner directly with START/STOP; the flame follows START (on while running, off when stopped). The min/max band is ignored in this mode.
- **Automatic** — the ESP regulates the burner to a min/max band on BT (F4); START/STOP runs/stops the process. Both limits are required to enter this mode.
- **Artisan** — the Artisan software owns the demand over MODBUS TCP (**Phase 2**). The front START/STOP button acts as a latched **emergency stop**.

Independent of the mode, the over-temperature cutoff (F4a), flame fault (LOCKOUT), and sensor fault always cut the burner — off always wins.

#### F4 — Temperature band (min/max)
> The gas-burner behaviour behind this feature (ignition, flame supervision, and the min/max regulation) is specified in `docs/design-flame-control.md`.
- Simple **min/max temperature band on BT**:
  - Below **min** → demand heat (burner on).
  - Above **max** → stop (burner off), with hysteresis between the two thresholds.
- **Off always wins:** reaching max, a flame fault, STOP, or a sensor fault always turns the burner off.

#### F4a — Independent over-temperature cutoff (`hard_max_temp_c`)
- A **hard ceiling on BT**, independent of the automatic min/max band, that applies in **every** mode (manual / automatic / Artisan). Optional — leave unset to disable.
- Reaching it **latches a LOCKOUT** (burner off, cleared only with a short press of the START/STOP button) — over-temperature is a serious event, above and beyond normal regulation. This is a safety backstop that also protects against a remote master (Artisan) commanding heat.
- Configurable in the web UI (Operation) and over serial (`hardmax <c>` / `hardmax -`); persisted to LittleFS.

#### F4b — Loop watchdog
- A task watchdog reboots the ESP if the control loop stalls; on reboot the burner-enable output is forced off before anything else, so a stall closes the burner (fail-safe).

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
- The ESP creates its own network `{Brand}` (`Torrador` for the default brand, per §0.2); default IP `192.168.4.1`
- **Captive portal** via `DNSServer` answering all DNS queries with the ESP's IP — the phone opens the configuration page automatically on connect
- Configuration page served from LittleFS, containing:
  - SSID (with a "scan networks" button via `WiFi.scanNetworks()`)
  - Network password
  - mDNS name (optional; default `torrador`)
- On save: credentials persisted to LittleFS (`config.json`) -> restart in operation mode

**Configuration reset (2 complementary mechanisms):**
1. **Automatic fallback:** connection failure after timeout -> AP mode (covers router/password change)
2. **Web interface button:** "Reset network configuration" option in operation mode

> The BOOT button is deliberately **not** used for network reset — it is inaccessible once the controller is installed in the equipment.

#### F6a — Optional web-UI password (HTTP Basic)
Prevents casual access to the admin interface from other devices on the network — especially in STA mode, where anyone can reach the ESP by IP or `torrador.local` (mDNS).
- **Optional:** a single credential stored in `config.network` (empty password = disabled). The customer decides: trusted network -> leave open for speed; shared/open network -> set a password. The **username is configurable** (`adminUser`, default `admin` from `BRAND_ADMIN_USER`) and shown in the UI so the operator knows what to type at the browser prompt; the password (`adminPassword`) toggles the feature on/off.
- **HTTP Basic Auth**, native to ESPAsyncWebServer — no session/cookie/login page. When set, it gates the **entire** interface (dashboard, `/status`, config and burner commands); only the captive-portal `onNotFound` redirect stays open.
- Set/cleared in the **Settings › Security** tab (username field + masked "Mudar senha" password gate: keep / change / disable), applied live with no reboot. The username is returned by `/config` (not a secret); the password never is — only `has_admin_password`.
- The Security tab also surfaces a **plain-language disclaimer**: operating the roaster over the web is as accessible as its physical buttons (anyone on the network can act, like anyone standing at the machine), with a "Saiba mais" note recommending AP mode + a strong password + a private network for security-critical installs.
- **Recovery** (forgotten password): the serial `auth` command (`auth user=.. pass=..` to set, `auth off` to disable — requires physical USB access).
- Conscious limits (V0): plain HTTP means the base64 credential is sniffable on the LAN — this closes the "anyone opens the IP" gap, not an on-path attacker; the traffic is already cleartext by design. Credentials stored in plaintext, like the Wi-Fi passwords. No rate-limiting/lockout, single user. Revisit if a stronger posture is needed.

### 2.2 Web interface — screen/section structure

| Section | Elements |
|---|---|
| **Dashboard** | Real-time BT and ET reading; current control mode; current burner state (idle/heating/hold/lockout/estop/sensor-fault); process running indicator; START/STOP the process (or emergency stop in Artisan); reset a lockout |
| **Settings › Operation** | Select the control mode (manual / automatic / Artisan); set the min/max band (°C) for automatic mode |
| **Settings › Network** | AP/STA mode, Wi-Fi scan + credentials, mDNS name, AP password (see F6) |
| **Settings › Security** | Optional web-UI password (HTTP Basic, blank = open — see F6a); independent over-temperature cutoff (°C, blank = disabled) |

*(Note: user-facing labels are in Portuguese (BR) for V0 per the Language Policy; the table above describes function in English.)*

### 2.3 Access requirements
- Access via browser on a computer or phone **on the same Wi-Fi network**
- Friendly address: `http://torrador.local` (mDNS), plus the direct IP
- **Plain HTTP, no HTTPS** — conscious decision: negligible risk on a local network; Chrome warnings ("Always Use Secure Connections", from Oct/2026) do not apply to local/private-network IPs
- **HTTP Basic Auth** (optional, implemented — see F6a): a minimal layer to prevent undue access from other devices on the network. Disabled by default; when a password is set it gates the entire interface

---

## 3. Technical Specifications

### 3.1 Hardware (Phase 1 / V0)

| Component | Qty | Function | Notes |
|---|---|---|---|
| ESP32 (WROOM) | 1 | Central controller | Dual-core and ~520KB RAM for the async web server + control logic simultaneously (and MODBUS in Phase 2) — see section 1.2.1 |
| MAX6675 (breakout) | 2 | Type-K thermocouple -> digital converter | SPI; range 0–1024°C; resolution 0.25°C; ~1 read every 220ms. Conscious decision to keep MAX6675 (simplicity) instead of MAX31855 (which would add thermocouple fault detection) — revisit in the future |
| Type-K thermocouple probe | 2 | BT: long/immersion probe in the mass; ET: short probe/air | Verify probe stem length compatible with the drum before purchasing |
| Relay module | 1 | Burner enable (power-gates the INV-27109) | In V0, **no load connected to the contacts** — the module's own indicator LED serves as visual feedback |
| Protoboard + jumpers | 1 | Prototype assembly | — |

### 3.2 Connections (logical definition)

- **MAX6675 (x2):** shared SPI — same SCK and SO for both chips; **individual CS** per chip (2 CS pins)
- **Relay (x1):** 1 digital GPIO for the burner-enable relay (HIGH/LOW). Note: common relay modules are active-LOW — confirm polarity
- Specific ESP pins: to be defined at implementation (avoid boot/strapping pins)

### 3.3 Firmware architecture

**Main modules:**

1. **Sensor reading:** periodic read cycle of the two MAX6675 via SPI (respecting the chip's ~220ms minimum interval)
2. **Temperature control:** min/max hysteresis on BT per read cycle:
   ```
   Demand source by mode: manual -> follow START; auto -> min/max band; artisan -> MODBUS
   Auto band:  BT <= min -> demand heat;  BT >= max -> stop;  in between -> keep state
   Off always wins (max reached, over-temp cutoff, flame fault, STOP/e-stop, sensor fault -> burner off)
   ```
3. **Web server:** ESPAsyncWebServer + async TCP (async — mandatory so as not to block the other tasks)
4. **File system:** LittleFS for (a) interface static files (HTML/CSS/JS) and (b) settings persistence (min/max)
5. **mDNS:** ESPmDNS library, registering `torrador.local`
6. **Network provisioning:** AP + captive portal state machine (see F6)

**Configuration (logical model):** a top-level `mode` plus per-mode and safety groups (see `docs/design-flame-control.md` §7 and `src/config.h`)
```
mode: "manual" | "auto" | "artisan"   // active control authority (default "manual")
auto.temperature = {                   // used only by "auto" mode
  min_c: number | null,                // null = not configured
  max_c: number | null                 // min_c < max_c; both required for "auto"
}
safety.hard_max_temp_c: number | null  // independent over-temp cutoff, ALL modes; null = disabled
artisan: { ... }                       // MODBUS options (Phase 2; scaffolded, no fields yet)
```

**Global system state:**
```
{
  mode: "manual" | "auto" | "artisan",
  process: bool,           // START/STOP latch
  burnerState: "idle" | "run" | "hold" | "lockout" | "estop" | "fault",
  temperature: { min_c, max_c },        // auto-mode band
  hard_max_c: number | null,            // over-temp cutoff
  bt: float,               // last reading (°C)
  et: float                // last reading (°C)
}
```
- `estop` is the latched emergency stop (Artisan mode, front button); `lockout` is a latched flame/over-temp fault; both clear only with BOOT.

### 3.4 HTTP API (routes)

**Operation mode:**

| Route | Method | Function |
|---|---|---|
| `/` | GET | Dashboard page (served from LittleFS) |
| `/settings` | GET | Settings page (operation + network + security) |
| `/status` | GET | JSON: mode, net, ssid, BT, ET, burner state, process on/off, band |
| `/brand` | GET | JSON: product name (white-label) |
| `/config` | GET | JSON bootstrap for the settings page (operation + network + `has_admin_password`) |
| `/command` | POST | `cmd=startstop` (START/STOP or e-stop) or `cmd=clear` (reset a latch) |
| `/operation` | POST | Save control mode + min/max band (applies live, persisted) |
| `/security` | POST | Save the over-temp cutoff (hard_max) + optional UI password (applies live, persisted; see F6a) |
| `/save` | POST | Save network config (mode/SSID/password/AP password/mDNS) and reboot |

*(All routes above require the UI password when one is set (F6a); only the captive-portal redirect stays open. Network reset — F6 mechanism 3 — is done from the settings page by switching to AP mode, which reboots the device as its own access point.)*

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

### 4.1 Phase 2 — Artisan integration (MODBUS TCP)
- ESP = MODBUS TCP server/slave; Artisan = client/master. Built on top of the validated bench base, **still dry** (no gas): Artisan drives the same burner-enable output the manual/auto modes already drive.
- The `artisan` control mode is already scaffolded end-to-end (mode selection, UI, front-button emergency stop); this phase adds the MODBUS transport and wires the burner-power register into the burner demand.
- Register map already defined:

| Register | Type | Address | Content | Status |
|---|---|---|---|---|
| Input Register | Read | 0 | BT (°C × 10) | Planned |
| Input Register | Read | 1 | ET (°C × 10) | Planned |
| Holding Register | Write | 0 | Burner power 0–100 | Planned |

- Temperature values transmitted as integer ×10 (MODBUS uses 16-bit registers, no decimals); Artisan configured with the corresponding divisor.
- **Burner power → actuation (this phase): simple on/off by threshold** (`power > threshold` ⇒ demand heat). The burner is a single on/off valve, so proportional **time-proportioning is deferred to Phase 3** — with the real INV-27109, every gas on-cycle re-runs the ignition sequence, so the modulation strategy must be designed against the real controller's behaviour, not a bench LED.
- **Safety precedence still holds:** the over-temperature cutoff (`hard_max_temp_c`), a flame fault (LOCKOUT), a sensor fault, and the front-button emergency stop (ESTOP) all cut the burner above whatever Artisan commands — off always wins.

### 4.2 Phase 3 — Real system / gas (real loads)
- **Burner:** **single, on/off** solenoid valve (not proportional), **110 VAC, normally-closed** (already sourced — see `docs/design-flame-control.md` §12). Driven by the real INV-27109; the ESP enable relay power-gates the INV's mains (power off ⇒ gas closes).
- This phase assembles the real flame controller and gas path (INV-27109, RC snubber across the valve coil, mains-rated enable relay, PC817 fault opto) and validates real ignition/flame supervision. Optional: revisit burner-power modulation (time-proportioning) now that the INV's re-ignition-per-cycle behaviour can be measured.

> **Out of initial scope — advanced automation.** Drum-motor automation (on/off,
> and any variable-speed control via AC+VFD or DC+PWM), ventilation control (PWM
> on a DC motor), and any other more advanced automation are **deliberately out
> of the initial scope**. V0 controls only the burner. These may be revisited in
> a later phase; they are intentionally left undefined here to avoid rework.

---

## 5. Registered Pending Items

| # | Pending item | Blocks V0? | Depends on |
|---|---|---|---|
| 1 | ~~Burner solenoid coil voltage~~ **Resolved: 110 VAC, normally-closed** (design-flame-control §12) | No | — |
| 2 | WebSocket vs. polling on the dashboard — resolved: **polling** `/status` every 1 s | No | — |
| 3 | ~~HTTP Basic Auth: include in V0 or not~~ **Resolved: optional HTTP Basic, whole UI (F6a)** | No | — |

---

## 6. V0 Acceptance Criteria

1. Web interface reachable at `http://torrador.local` from a computer and phone on the same network
2. Dashboard displays BT and ET in real time, with plausible values from the real probes
3. In **manual** mode, START/STOP runs and stops the process; the flame follows START (burner off on STOP and at boot; LOCKOUT cleared with BOOT)
4. In **automatic** mode with min/max set, the burner regulates to the band on BT (off always wins)
5. The control mode, min/max band, and over-temperature cutoff can be set, cleared, and saved (web UI and serial)
6. The mode / min/max / hard_max settings persist after restarting the ESP
7. Switching control mode cuts the burner first (no inherited running flame)
7a. The independent over-temperature cutoff (`hard_max_temp_c`) latches a LOCKOUT in every mode when BT reaches it, regardless of the band or an Artisan command
7b. A stalled control loop reboots via the watchdog, and the burner-enable output comes up de-energized (fail-safe)
8. With no saved credentials, the device comes up in AP mode (`Torrador`) and the captive portal opens the configuration page automatically when connecting from a phone
9. After saving SSID/password in the portal, the device restarts and connects to the configured network
10. Connection failure after timeout automatically returns to AP mode; a web interface option also resets the network configuration
11. User-facing interface text is in Portuguese (BR), with strings structured to allow future English localization
12. The product name is not hardcoded: it comes from the branding config, and changing it (plus rebuilding) updates the web UI, serial boot banner, mDNS default hostname, and AP SSID (white-label, per §0.2)
