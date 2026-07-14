# Design & ADR — Flame / Burner Control (gas)

**Status:** Decisions closed, ready for implementation
**Phase goal:** Add safe gas-burner actuation (valve + spark + flame supervision)
plus a simple min/max temperature hold, configured over the serial console.
**Relates to:** `PRD-torrador-esp32-v0.md` (F2/F3/F4 burner control, safety rules)
**Last updated:** 2026-07-14

---

## 1. Context & goal

Up to now the "burner" was an abstract on/off relay. This phase turns it into a
real **gas burner controller** with combustion supervision:

- Drive a gas **solenoid valve** and a **spark generator** to light the flame.
- Prove the flame with an **ionization sensor** and keep gas open **only** while
  flame is proven (or during a short, bounded ignition trial).
- Persist the parameters needed to manage all of this, editable over the
  **serial console**.
- On top of it, hold a temperature within a configured **min/max band** by
  cycling the burner on/off.

Guiding safety principle: **gas is open only when flame is proven, or during a
bounded ignition trial.** Everything below derives from this.

## 2. Scope

**In scope**
- Gas heat source only (electric reserved as a stub in the abstraction).
- Direct ignition of a single main valve (no pilot).
- Ignition state machine with retries and a latched lockout.
- Ionization-based flame supervision (fast, near-boolean signal).
- Min/max temperature hold, expressed as the two-threshold subset of the rule
  engine.
- Serial command interface for configuration and lockout reset.
- **Dry run only** — relays/LEDs as actuators, **no real gas connected**; flame
  simulated over serial. Real-gas commissioning is a later, separately gated step.

**Out of scope (this phase)**
- Electric resistive heat source (reserved only).
- Pilot flame, valve proving, pre/post purge (no ventilation control in V0).
- Full rule engine (up to 5 conditions, AND/OR) — only the min/max subset now.
- Flame modulation / time-proportioning (Phase 3).
- Web UI for configuration (serial only this phase).

## 3. Decisions (ADR)

| # | Decision | Rationale |
|---|---|---|
| D1 | Flame detection by **ionization rod** (flame rectification) | Industry standard; detects presence/loss in ms. Temperature-based sensing is too slow for flame-loss safety. |
| D2 | **Direct ignition, single main valve** (no pilot) | Simplest topology; matches the available hardware. Each "on" is a full ignition of the main burner. |
| D3 | Spark is **continuous while energized** (gated) | Matches typical igniter modules; firmware only holds one output on for `spark_duration`. |
| D4 | Regulation sensor is **configurable, default BT** | BT is the only channel wired today (step 1); keep it a config field so ET can be selected later without a rebuild. |
| D5 | **Lockout reset via BOOT button** (short press at runtime) | Recovers without a terminal in real operation. Boot-time hold stays reserved for the future network reset (PRD F6). |
| D6 | **No pre/post purge** — only the inter-trial delay | No ventilation control in V0; an active purge isn't possible. The mandatory gas-closed delay between retries is the safety-relevant wait. |
| D7 | Min/max band is **a case of the rule engine** | One demand model: activation = `temp < min`, deactivation = `temp > max`, deactivation wins ties (existing safety rule). |
| D8 | Implement the **min/max subset now**, rule-engine-shaped | Meets the phase goal fast; the full engine (5 conditions, AND/OR) slots into the same demand layer later. |
| D9 | Configuration via **serial command grammar** | Scriptable, easy to extend and log; no navigation state. Web UI comes later. |
| D10 | Flame reading behind a **`FlameSensor` abstraction** | The electrical front-end (digital comparator vs analog ADC) is chosen at hardware-assembly time; firmware does not block on it. |

## 4. Architecture & abstractions

Two levels, deliberately separated:

- **Demand** — "is heat wanted?" Produced by the rule engine (min/max subset in
  this phase). Pure function of temperature; outputs a boolean burner demand.
- **Actuation** — "how to produce/stop flame safely?" The `HeatSource` executes
  the demand.

```
temperature ──► [ demand layer / rule engine (min<->max) ] ──► burner demand (bool)
                                                                     │
                                                                     ▼
                                              [ HeatSource: GasBurner state machine ]
                                                 drives valve + spark, reads FlameSensor
```

Interfaces (English identifiers per language policy):

- `HeatSource`: `request(bool on)`, `tick()`, `State state()`.
  - `GasBurner` implements the ignition state machine (§5).
  - `ElectricHeater` — reserved **stub** only (would be a plain relay + its own
    over-temperature cutoff).
- `FlameSensor`: `bool present()` — implementation (digital/analog) chosen later
  (D10). A serial-driven fake backs the dry-run tests.
- Demand layer: rule-engine-shaped, but only min/max threshold logic;
  **deactivation is evaluated first and wins ties.**

## 5. Ignition state machine (GasBurner)

| State | Valve | Spark | Meaning |
|---|---|---|---|
| `OFF` | closed | off | Idle, no demand |
| `IGNITING` | **open** | gated | Ignition trial; waiting for proven flame within the safety time |
| `RUN` | open | off | Flame proven; regulating temperature |
| `INTERTRIAL` | closed | off | Gas-closed wait between retries (lets gas dissipate) |
| `LOCKOUT` | closed | off | Latched after retries exhausted — **manual reset only** |
| `FAULT` | closed | off | Temperature sensor fault → safe state |

Transitions:

```
OFF ──(demand rises)──────────────► IGNITING
IGNITING ──(flame proven)─────────► RUN
IGNITING ──(trial timeout, retries left)──► INTERTRIAL ──(delay done)──► IGNITING
IGNITING ──(trial timeout, no retries left)──► LOCKOUT
RUN ──(demand falls: temp > max)──► OFF
RUN ──(flame lost > loss_debounce)──► on_loss = retry → INTERTRIAL
                                    │            = lockout → LOCKOUT
LOCKOUT ──(BOOT short press)──────► OFF
any + temp-sensor fault ──────────► FAULT ──(fault clears)──► OFF
```

### Ignition timing (IGNITING)

`t0` = valve opens.

```
t0 = valve opens
 │
 ├─ spark_offset ∈ [-5s, +5s] ──► spark ON at (t0 + spark_offset), for spark_duration
 │
 ├──────────── trial_for_ignition (safety time) ─────────────►│
 │             (max time gas is open WITHOUT proven flame)     │
 │   ▲ flame proven before the deadline → RUN (spark off, valve stays)
 │   else at the deadline → close valve → INTERTRIAL
```

`trial_for_ignition` is the hard ceiling on "gas open without flame" and takes
priority: if `spark_offset` would delay the spark so much that flame can't be
proven in time, the configuration is invalid (reject on `set`/`save`).

## 6. Configuration model

Persisted as JSON on LittleFS, loaded at boot; schema carries `config_version`
for future migration.

```jsonc
{
  "config_version": 1,
  "heat_source": "gas",            // "gas" | "electric" (reserved, stub)

  "ignition": {
    "spark_offset_s": 0.0,         // -5.0 .. +5.0, spark start relative to valve open
    "spark_duration_s": 3.0,       // > 0
    "trial_for_ignition_s": 5.0,   // > 0, ceiling on gas-open-without-flame
    "retry_max": 3,                // >= 0
    "intertrial_delay_s": 10       // >= 0, gas closed between retries
  },

  "flame_supervision": {
    "on_loss": "retry",            // "retry" | "lockout"
    "loss_debounce_ms": 500        // >= 0, absent-signal time before declaring loss
  },

  "regulation": {                  // == two rules (activate < min, deactivate > max)
    "enabled": true,
    "sensor": "BT",                // "BT" | "ET"
    "temp_min_c": 190,             // temp_min_c < temp_max_c
    "temp_max_c": 205,
    "min_on_time_s": 15,           // anti short-cycling
    "min_off_time_s": 15
  },

  "safety": {
    "hard_max_temp_c": 250,        // independent over-temperature cutoff
    "sensor_fault_action": "shutdown"
  }
}
```

Validation (rejected on set/save): `temp_min_c < temp_max_c`; `spark_offset_s`
in [-5, +5]; positive durations where noted; the spark window must be able to
prove flame within `trial_for_ignition_s`.

## 7. Serial command interface

Line-based command grammar over the serial console:

| Command | Effect |
|---|---|
| `show` | Print the whole configuration (and current state) |
| `get <key>` | Print one value, e.g. `get ignition.spark_duration_s` |
| `set <key> <value>` | Set one value (validated); in-memory until `save` |
| `save` | Persist the configuration to LittleFS |
| `reset lockout` | Clear a latched lockout (equivalent to the BOOT press) |
| `sim flame on\|off` | **Dry-run only:** drive the fake flame signal |

Keys are dotted paths matching the schema (`section.field`). Invalid keys/values
return an error line and leave the config unchanged.

## 8. Safety invariants (non-negotiable)

1. **Fail-safe valve:** normally-closed, opens only when energized. Firmware
   forces all actuator outputs OFF at the very start of `setup()`, before
   anything else, so reboot/brownout/crash leaves gas closed.
2. **Watchdog:** if the loop stalls with gas open, the watchdog resets and the
   valve de-energizes (closes). The cooperative loop must never block in `RUN`.
3. **`trial_for_ignition` is the ceiling** on gas-open-without-flame and has
   priority over spark timing.
4. **Sensor faults fail safe:** a temp-sensor fault (open thermocouple) → `FAULT`
   with gas closed; **an absent flame signal is always treated as no flame**.
5. **Lockout is latched** — cleared only by a deliberate human action (BOOT
   press / `reset lockout`); never by an automatic restart.
6. **Independent over-temperature cutoff** (`hard_max_temp_c`) shuts the burner
   regardless of the regulation band, covering a stuck-open valve or a failed
   loop.
7. **Mechanical backstop recommended** on the gas line (manual shutoff /
   mechanical thermostat) — software is not the only protection layer in
   combustion.

## 9. Dry-run validation (no real gas)

- Valve and spark are relays/LEDs; **no gas connected**.
- Flame is the serial-driven fake (`sim flame on|off`), exercising
  `IGNITING → RUN → loss → retry → LOCKOUT` and the timing.
- Regulation is exercised by heating the real BT thermocouple across the band.
- Verify: on boot/reset the valve output is de-energized before anything else;
  watchdog closes "gas" on an induced hang.

## 10. I/O and pin proposal

New I/O for this phase (added to `board_config.h`):

| Signal | Direction | Proposed pin | Note |
|---|---|---|---|
| Gas valve | output | **GPIO25** | reuses the existing "burner relay"; active-LOW, NC valve |
| Spark generator | output | **GPIO27** | gated on/off |
| Flame (ionization) | input | **GPIO34** | input-only, ADC1-capable → works as digital or analog (D10) |

Existing (unchanged): MAX6675 SCK 18 / SO 19 / CS_BT 5 / CS_ET 17, OLED I2C
21/22, drum relay 26, BOOT 0. Avoid strapping pins for new outputs.

## 11. Open items (hardware / commissioning — do not block firmware design)

- **H1** — Driver stage and coil voltages (valve, spark module): relay vs SSR,
  and fail-safe NC wiring.
- **H2** — Ionization front-end: digital (comparator → GPIO) vs analog (ADC).
  Deferred behind the `FlameSensor` abstraction (D10).
- **H3** — Final pin assignment (§10 is a proposal).
- **C1** — Default timer/retry values (§6 are proposals); tune on the bench.

## 12. Acceptance criteria (this phase)

1. Config round-trips to LittleFS and survives reboot; `show`/`get`/`set`/`save`
   work with validation.
2. On demand (`temp < min`), the ignition sequence runs: valve opens, spark is
   gated per `spark_offset`/`spark_duration`; simulated flame within
   `trial_for_ignition` → `RUN` (spark off, valve stays open).
3. No flame within `trial_for_ignition` → valve closes, waits
   `intertrial_delay`, retries up to `retry_max`, then `LOCKOUT`.
4. `LOCKOUT` clears only via BOOT short press / `reset lockout`; no auto-restart.
5. In `RUN`, simulated flame loss beyond `loss_debounce_ms` closes the valve
   immediately; behavior follows `on_loss`.
6. Regulation holds the configured sensor within `[temp_min_c, temp_max_c]`;
   `min_on_time_s`/`min_off_time_s` prevent short-cycling.
7. `hard_max_temp_c` forces shutdown regardless of the band.
8. A temp-sensor fault (open thermocouple) drives `FAULT` with the valve closed.
9. On boot/reset/brownout the valve output is de-energized before anything else;
   an induced loop hang closes "gas" via the watchdog.
10. All actuation is dry (relays/LEDs); no real gas is connected.
