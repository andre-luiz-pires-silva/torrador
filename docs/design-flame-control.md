# Design & ADR — Flame / Burner Control (gas)

**Status:** Decisions closed. Bench control flow implemented (dry); real gas actuation is the final phase.
**Goal:** Safe gas-burner actuation by **delegating combustion to a dedicated
flame controller (Inova INV-27109)**, plus a simple min/max temperature hold and an
independent over-temperature cutoff. Configuration is available over **both the serial
console and the web UI** (the web UI landed after this doc's first draft).
**Phase note (2026-07):** the roadmap was reordered — **Artisan integration is now
Phase 2** (dry, on the validated base) and the **real INV + gas assembly is Phase 3**
(the last step). This document specifies the burner/flame design that Phase 3 realizes
with real hardware; the control logic itself already runs dry on the bench.
**Relates to:** `PRD-torrador-esp32-v0.md` (F3 modes, F4/F4a burner control + over-temp, safety rules)
**Controller manual:** `docs/manuals/Manual_INV_27109v9.1.pdf`
**Last updated:** 2026-07-14

---

## 1. Context & goal

The burner is driven by a **dedicated, purpose-built flame controller** — the
**Inova INV-27109** — which internally handles the dangerous, real-time parts of
combustion:

- Drives the **spark generator** (S1) and the **gas solenoid valve** (S2).
- Senses flame by **ionization** and closes gas on flame loss.
- Runs a fixed ignition sequence and a start-up short-circuit test.

The **ESP32 orchestrates**: it decides *when* heat is wanted (min/max band),
**enables** the controller, **supervises** it (via the controller's fault
output), latches a safety lockout, and drives the display/serial UI.

Guiding principle unchanged: **gas is open only under proven flame** — but now the
flame proving and the immediate gas cutoff are the controller's job, not ours.
The manufacturer explicitly states the INV-27109 **must not be used alone as a
safety system**, so the ESP adds a master lockout. An independent mechanical
safety backstop on the gas line is strongly recommended at the installation, but
it is **outside the scope of this firmware/controller** (see §9 and §12).

## 2. Scope

**In scope**
- Gas heat source via the INV-27109 (electric reserved as a stub).
- ESP **enables** the controller by power-gating its mains supply through a relay.
- ESP **supervises** via the controller's 12V fault output, read through a
  **PC817 optocoupler**; latched master lockout on failure.
- Simple **min/max** temperature regulation on BT (below min → heat, above max →
  stop). If min/max are not set, the burner stays on directly.
- Serial command interface for configuration and lockout reset.
- **Dry run** — no real gas connected; the fault/flame input exercised with a
  push-button (or the real INV, which faults with no gas).

**Out of scope (this design)**
- Electric resistive heat source (reserved only).
- Flame modulation / time-proportioning (deferred to Phase 3, measured against the real INV).
- Ignition timing / purge parameters — these live **inside the INV-27109** and
  are not ours to configure.

*(Note: configuration was originally serial-only; the web UI + LittleFS persistence
now exist and are the primary configuration surface — serial remains as a fallback.)*

## 3. Hardware & reference devices

- **Inova INV-27109** — flame controller. 85–250 VAC; 1 flame-sensor (ionization)
  input; **2 relay outputs 5A/220VAC** — S1 = spark unit ("usina"), S2 = gas
  solenoid valve; **1 buzzer output 12VDC / 20mA max** (fault indicator). Fixed
  ignition cycle: spark 5s on / 3s off, **3 attempts** (~24s window); holds S2
  while flame is sensed; on failure turns outputs off and asserts the buzzer.
  Start-up short-circuit test. **Does not latch** (retries on flame absence).
- **PC817 optocoupler** — reads the 12V fault output into a 3.3V ESP GPIO with
  galvanic isolation (see §10).
- **Mains-rated relay** — the ESP "enable": gates the INV's 85–250VAC supply.
- Driven by the INV (owned by the operator's hardware): the spark unit ("usina"),
  the AC **gas solenoid valve** (**110 VAC, normally-closed** — matches the mains
  fed to the INV), the ionization electrode, and the spark electrode.
- **RC snubber** across the solenoid-valve coil (inductive load on a
  resistive-rated relay — protects contacts, reduces EMI on the ionization sense).

## 4. Decisions (ADR)

| # | Decision | Rationale |
|---|---|---|
| D1 | **Delegate combustion to a dedicated controller (INV-27109)** — Architecture B | Ignition timing, ionization sensing, spark/valve drive and short-circuit test are done by a purpose-built device; the ESP orchestrates. Simpler, safer firmware for a commercial white-label product. |
| D2 | Flame detection by **ionization**, performed **inside the INV-27109** | Industry-standard fast sensing; the high-voltage front-end lives in the module, not on our board. |
| D3 | **Direct ignition, single gas valve** (no pilot), driven by the INV | Simplest topology; the INV cycles the spark (S1) and holds the valve (S2). |
| D4 | Control the INV by **power-gating its mains via a relay** (it has no enable input) | The INV runs when energized; the ESP relay is the call-for-heat and a hard master cutoff — **power off ⇒ gas closes**. |
| D5 | Read the INV fault (12V buzzer) via a **PC817 optocoupler** | Galvanic isolation from the mains-referenced 12V; clean 3.3V logic to the ESP. |
| D6 | **ESP-level master LOCKOUT**; reset via a short press of the START/STOP button | The INV does not latch and its manual says it must not be the sole safety system. The ESP latches and refuses to re-enable until manual reset. |
| D7 | Temperature regulation is a simple **min/max** band on **BT** | Below min → demand heat; above max → stop (hysteresis). If min/max are not both set, the burner stays on directly. Deliberately minimal — no rule engine, no BT/ET selection. |
| D8 | Configuration via **serial commands** (`min` / `max` / `show`) | Simple and scriptable; runtime now, LittleFS persistence later; web UI later. |
| D9 | Independent mechanical safety backstop — **out of scope** of this FW/controller (installer's responsibility) | Manufacturer says the INV must not be the sole safety system; a mechanical backstop is strongly recommended at installation, but this software cannot provide it. Documented, not implemented. |

## 5. Architecture & abstractions

```
temperature ─► [ min/max regulation on BT ] ─► burner demand (bool)
                                                                   │
                                                                   ▼
                                        [ HeatSource: GasBurner (thin supervisor) ]
                                          enable relay ─► INV-27109 ─► spark + valve + ionization
                                          reads INV fault (12V) ◄── via PC817 opto
```

- `HeatSource`: `request(bool on)`, `tick()`, `State state()`.
  - `GasBurner` is now a **thin supervisor**: it enables the INV (optimistically —
    no ignition/confirm step), watches the fault line, and latches the lockout.
  - `ElectricHeater` — reserved **stub** (a relay + its own over-temp cutoff).
- `FlameSensor`: `bool fault()` — reads the INV fault input (or the bench
  push-button). Fail-safe polarity; the module is the physical safety authority.
- Regulation: a simple min/max hysteresis on BT; if min/max are not configured,
  demand is always true (flame direct). Turning **OFF** (max reached, a fault, or
  STOP) always takes precedence.

## 6. Burner state machine (GasBurner)

| State | INV relay | Meaning |
|---|---|---|
| `IDLE` | open (INV off) | Process off / idle |
| `RUN` | closed (INV on) | Process on, burner firing (optimistic — flame assumed lit) |
| `HOLD` | open (INV off) | Process on, temperature satisfied (≥ max) — not firing |
| `LOCKOUT` | open (INV off) | Latched after a fault — **manual reset only** |
| `FAULT` | open (INV off) | Temperature-sensor fault → safe state |

Transitions:

```
IDLE ──(process START; demand)──────────► RUN
IDLE ──(process START; temp satisfied)──► HOLD
RUN  ──(fault asserted by INV)──────────► LOCKOUT
RUN  ──(demand falls: temp > max)───────► HOLD
RUN/HOLD ──(process STOP)───────────────► IDLE
HOLD ──(demand rises: temp < min)───────► RUN
LOCKOUT ──(START/STOP short press)──────► IDLE
any + BT ≥ hard_max_temp_c ──────────────► LOCKOUT   (over-temp cutoff; START/STOP to clear)
any + temp-sensor fault ────────────────► FAULT ──(fault clears)──► IDLE
```

*(In **Artisan** mode the front button is a latched **ESTOP** instead of START/STOP —
same "cut and refuse to re-enable until BOOT" semantics as LOCKOUT.)*

**Optimistic ignition:** the ESP does **not** wait to confirm flame — as soon as it
enables the INV it considers the burner firing (`RUN`). The INV owns ignition and
flame supervision; if it fails, its fault signal drives `RUN → LOCKOUT`. This keeps
the firmware simple and fits a future electric heat source (just switch it on).
There is no ESP-level retry — the INV's internal 3 attempts are the retries.

## 7. Configuration model

Settings live in a dedicated module (`src/config.h` / `src/config.cpp`), persisted
to LittleFS as `/config.json` (ArduinoJson). Loaded at boot; if the file is absent
or can't be read in the current format, it is treated as no config and reset to
defaults (no versioning/migration). A valid change over serial is saved immediately.

A top-level `mode` selects the control authority; per-mode and safety settings live
in their own groups. **Three modes** (see `src/config.h`):
- **`manual`** — the operator drives START/STOP directly; the flame follows START (no band).
- **`auto`** — the ESP regulates BT to a min/max band (both limits required).
- **`artisan`** — **MODBUS TCP slave** driven by Artisan (**Phase 2**; struct scaffolded, no fields yet).

Current schema:
```jsonc
{
  "mode": "manual",                  // "manual" | "auto" | "artisan"
  "auto": {                          // used by "auto" mode
    "temperature": {                 // min/max band on BT
      "min_c": 28,                   // °C  (absent = not configured)
      "max_c": 33                    // min < max
    }
  },
  "safety": {                        // applies in EVERY mode
    "hard_max_temp_c": 240           // independent over-temp cutoff (absent = disabled)
  },
  "network": { /* mode, ssid, password, ap_password, mdns_host — PRD F6 */ }
}
```

`safety.hard_max_temp_c` and `network` are **implemented**. Still planned (design intent,
not implemented): `burner.fault_debounce_ms`, `safety.sensor_fault_action`, and the
`artisan` MODBUS options (Phase 2).

Validation (rejected on set): `min_c < max_c`; `auto` mode requires both limits.

## 8. Serial command interface

| Command | Effect |
|---|---|
| `show` | Print the min/max config and current state |
| `min <c>` | Set the minimum temperature (°C) |
| `max <c>` | Set the maximum temperature (°C) |
| `min -` / `max -` | Clear a value (unset ⇒ flame direct) |
| `hardmax <c>` | Set the independent over-temperature cutoff (°C) |
| `hardmax -` | Clear the cutoff (disabled) |
| `mode manual\|auto\|artisan` | Select the control mode |
| `net ap\|sta ...` | Configure networking (see serial help) |

Invalid input returns an error line and leaves the config unchanged; a valid
change is saved to `/config.json` immediately. The LOCKOUT is cleared with the
**BOOT** button. (More commands and a web UI are planned follow-ups.)

## 9. Safety invariants (non-negotiable)

1. **The INV-27109 must not be the sole safety system** (manufacturer statement).
   The ESP therefore adds a **master lockout**. An **independent mechanical safety
   backstop** on the gas line (e.g. a thermocouple safety valve) is strongly
   recommended at the installation but is **out of scope of this firmware** — it is
   the integrator's/installer's responsibility (see §12).
2. **Power-off closes gas:** the enable relay de-energizes the INV, which drops S2
   (valve closed). The ESP forces the enable output OFF at the very start of
   `setup()`, so reboot/brownout/crash leaves gas closed.
3. **Master lockout is latched** — cleared only by a deliberate human action
   (a short press of the START/STOP button); never by automatic restart.
4. **Galvanic isolation** on the fault read (PC817) — never tie the INV's 12V
   ground to the ESP ground.
5. **Fault / absent signal fails safe:** any fault or missing flame signal is
   treated as no flame; the INV is the physical safety authority — it closes gas on
   flame loss regardless of what the ESP reads.
6. **Watchdog** cuts the enable (closes gas) if the loop stalls; the cooperative
   loop must never block in `RUN`. *(Implemented: the ESP32 task WDT reboots on a
   loop stall, and `setup()` forces the enable output OFF before anything else.)*
7. **Independent over-temperature cutoff** (`hard_max_temp_c`) drops the burner
   regardless of the regulation band. *(Implemented: latches LOCKOUT in every mode
   — manual/auto/artisan — when BT reaches the ceiling.)*
8. **Snubber** across the solenoid-valve coil (inductive load on a
   resistive-rated relay) — protects the INV's S2 contacts and keeps the
   ionization reading clean.

## 10. Wiring / I/O

**INV-27109 side** (mains, done by the operator's hardware):
- N/F 85–250 VAC in — **110 VAC in this build** — fed through the ESP's enable relay.
- S2 → gas solenoid valve (**110 VAC, normally-closed**) — **RC snubber across the
  coil**, e.g. ~100Ω + 100nF X2-class.
- S1 → spark unit ("usina").
- Ionization electrode → flame-sensor input.

**ESP32 side** (added to `board_config.h`):

| Signal | Direction | Pin | Note |
|---|---|---|---|
| INV enable | output | **GPIO4** | reuses the former "burner relay"; drives a **mains-rated** relay gating the INV's supply; OFF ⇒ gas closed |
| INV fault (12V via PC817) | input | **GPIO32** | `INPUT_PULLUP`; **active-LOW** (fault ⇒ LOW); PC817 with a 2.2kΩ LED resistor; bench push-button on the same node |

PC817 fault read:
```
INV 12V (buzzer) ─[2.2kΩ]─►|LED|─┐            ┌─ 3V3 (GPIO internal pull-up)
                                 │   PC817    │
INV 12V GND ─────────── LED cathode           ├─► GPIO32
                        transistor collector ─┘
                        transistor emitter ───► ESP GND
  fault active (12V) ⇒ LED on ⇒ transistor on ⇒ GPIO LOW
```
Freed by this architecture: the previous separate spark output (GPIO27) and the
dedicated ionization front-end input — the INV owns both. Unchanged: MAX6675
(SCK 18 / SO 19 / CS_BT 26 / CS_ET 27), OLED I2C 21/22, BOOT 0.
Pin numbers are **provisional** and live in `board_config.h` (easy to change);
they will be refined when the PCB is designed. For now the goal is the simplest
wiring that exercises the control flow.

## 11. Dry-run validation (no real gas)

- **Firmware-only:** the enable output drives an LED standing in for the INV; the
  push-buttons drive the process (START/STOP) and the fault/flame input. Exercise
  `IDLE → RUN ⇄ HOLD`, a fault → `LOCKOUT` → BOOT reset, and the min/max
  regulation (by heating the real BT thermocouple across the band).
- **With the real INV, still no gas:** energizing the INV runs its sequence and
  **faults** (no flame) — this exercises the real fault path, the opto read, and
  the ESP lockout end-to-end.
- Verify: on boot/reset the enable output is de-energized before anything else;
  an induced loop hang cuts the enable (closes "gas") via the watchdog.

## 12. Decisions & remaining details

**Resolved:**
- **Mains / valve (H1):** **110 VAC**; the gas solenoid valve is **110 VAC,
  normally-closed** (already sourced). Enable relay: a mains-rated relay module
  (~10A / 250VAC), driveable by the ESP.
- **Fault read (H2):** discrete **PC817** + 2.2kΩ LED resistor (see §10).
- **Pins (H3):** provisional — enable **GPIO4**, fault **GPIO32**; all pins live
  in `board_config.h` and are trivial to change. Goal now: the simplest wiring to
  exercise the flow; refine at PCB design.

**To finalize at the bench / PCB stage:**
- RC snubber values on the valve coil.
- Final pin map and driver details when the PCB is designed.

**Explicitly OUT OF SCOPE of this firmware (H4):**
- The **independent mechanical safety backstop** on the gas line (e.g. a
  thermocouple safety valve). This firmware/controller **cannot** provide it. It
  is documented as strongly recommended/required at the installation, but it is
  the responsibility of the product integrator/installer — **not this software**.

## 13. Acceptance criteria (this phase)

1. Min/max are set over serial (`min`/`max`/`show`); unset ⇒ the burner stays on
   directly and the display shows "-".
2. On demand (`temp < min` or unset), the ESP enables the INV and the burner goes
   to `RUN` immediately (optimistic); a later fault drops it to `LOCKOUT`.
3. A fault (12V) during ignition or run drops the burner to `LOCKOUT` and cuts the
   enable (gas closed).
4. `LOCKOUT` clears only via a short press of the START/STOP button; no auto-restart.
5. When demand falls (`temp > max`) the ESP cuts the enable (back to `HOLD`).
6. `hard_max_temp_c` forces shutdown regardless of the band.
7. A temp-sensor fault (open thermocouple) drives `FAULT` with the enable off.
8. On boot/reset/brownout the enable output is de-energized before anything else;
   an induced loop hang cuts the enable via the watchdog.
9. The fault read is opto-isolated; the INV 12V ground is not tied to the ESP.
10. All actuation is dry (LED/relay + push-button, or the real INV with no gas).
