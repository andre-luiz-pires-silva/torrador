#include <Arduino.h>
#include <U8g2lib.h>
#include <esp_task_wdt.h>
#include "board_config.h"
#include "branding.h"
#include "config.h"
#include "net.h"

// -----------------------------------------------------------------------------
// Burner control — bench version (dry, no gas, no INV yet).
//
// Combustion is delegated to the INV-27109 (see docs/design-flame-control.md), so
// the ESP is optimistic: as soon as it enables the controller it considers the
// burner firing, and reacts to a fault afterwards. There is no ignition/confirm
// step on our side — the INV owns ignition. This also fits a future electric
// heat source (just switch the element on).
//
// Bench hardware:
//   START/STOP button (GPIO33)  -> process on/off (toggle)
//   FLAME-FAULT button (GPIO32) -> flame fault, aligned to the INV alarm signal
//   Enable output (GPIO25)      -> LED (stands in for the INV enable relay)
//   BOOT button (GPIO0)         -> reset a latched LOCKOUT
//   MAX6675 (BT)                -> temperature for min/max regulation
//
// Temperature control is a simple min/max band on BT. If min and max are not both
// configured, the burner stays ON directly. Set over serial (`min <c>`, `max <c>`,
// `min -`, `max -`, `show`); values are persisted to LittleFS (see config.h).
//
// States: IDLE -> RUN <-> HOLD, with LOCKOUT (latched fault) and FAULT (sensor).
// -----------------------------------------------------------------------------

// ---- Display ----
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);

// ---- Debounced buttons ----
static const uint32_t DEBOUNCE_MS = 30;
struct Button { uint8_t pin; bool activeHigh; bool level; bool rawPrev; uint32_t tMark; };
static Button btnStart = { PIN_START_STOP,  true,  false, false, 0 };
static Button btnFault = { PIN_FLAME_FAULT, true,  false, false, 0 };
static Button btnBoot  = { PIN_BOOT_BUTTON, false, false, false, 0 };  // active-low (INPUT_PULLUP)

// Update debounce; return true on a rising edge (released -> pressed).
static bool btnUpdate(Button &b, uint32_t now) {
  bool raw = (b.activeHigh ? digitalRead(b.pin) == HIGH : digitalRead(b.pin) == LOW);
  bool prevLevel = b.level;
  if (raw != b.rawPrev) { b.rawPrev = raw; b.tMark = now; }
  else if ((now - b.tMark) >= DEBOUNCE_MS) { b.level = raw; }
  return (!prevLevel && b.level);
}

// ---- State ----
enum class State { IDLE, RUN, HOLD, LOCKOUT, ESTOP, FAULT };
static State state = State::IDLE;
static bool  processOn = false;
static bool  demand = false;

static const uint32_t SENSOR_INTERVAL_MS = 1000;
static uint32_t lastReadMs = 0;
static float lastBtC = NAN;   // bean temperature (BT) — drives the min/max control
static float lastEtC = NAN;   // air/exhaust temperature (ET) — telemetry only

static const char *stateName(State s) {
  switch (s) {
    case State::IDLE:    return "IDLE";
    case State::RUN:     return "RUN";
    case State::HOLD:    return "HOLD";
    case State::LOCKOUT: return "LOCKOUT";
    case State::ESTOP:   return "ESTOP";
    case State::FAULT:   return "FAULT";
  }
  return "?";
}

static float max6675ReadCelsius(uint8_t csPin) {
  digitalWrite(csPin, LOW);
  delayMicroseconds(2);
  uint16_t value = 0;
  for (int8_t bit = 15; bit >= 0; bit--) {
    digitalWrite(PIN_MAX6675_SCK, HIGH);
    delayMicroseconds(2);
    if (digitalRead(PIN_MAX6675_SO)) value |= (1u << bit);
    digitalWrite(PIN_MAX6675_SCK, LOW);
    delayMicroseconds(2);
  }
  digitalWrite(csPin, HIGH);
  if (value & 0x0004) return NAN;   // D2 set -> thermocouple open
  value >>= 3;
  return value * 0.25f;
}

// ---- Serial configuration ----
static void fmtTemp(char *out, size_t n, float v) {
  if (isnan(v)) snprintf(out, n, "-");
  else          snprintf(out, n, "%g", v);
}

// Display formatter: fixed-width, always 3 integer digits + 1 decimal, e.g.
// "017.5" (or "---.-" when unread). Constant width keeps the OLED layout stable
// for any temperature — no shifting as values grow to three digits.
static void fmtFixed(char *out, size_t n, float v) {
  if (isnan(v)) snprintf(out, n, "---.-");
  else          snprintf(out, n, "%05.1f", v);
}

static void printConfig() {
  Serial.print(F("[cfg] mode=")); Serial.println(modeName(config.mode));
  char sMin[8], sMax[8];
  fmtTemp(sMin, sizeof(sMin), config.automatic.temperature.minC);
  fmtTemp(sMax, sizeof(sMax), config.automatic.temperature.maxC);
  Serial.print(F("[cfg] min="));  Serial.print(sMin);
  Serial.print(F("  max="));      Serial.print(sMax);
  Serial.print(F("  -> "));
  Serial.println(config.automatic.temperature.configured() ? F("min/max") : F("flame direct"));

  char sHard[8];
  fmtTemp(sHard, sizeof(sHard), config.safety.hardMaxC);
  Serial.print(F("[cfg] hard_max=")); Serial.println(sHard);

  Serial.print(F("[cfg] net=")); Serial.print(netModeName(config.network.mode));
  if (config.network.mode == NetMode::STA) {
    Serial.print(F("  ssid=")); Serial.print(config.network.ssid);
    Serial.print(F("  pass=")); Serial.print(config.network.password[0] ? F("(set)") : F("(none)"));
  } else {
    Serial.print(F("  appass=(set)"));
  }
  Serial.print(F("  mdns=")); Serial.println(config.network.mdnsHost);
}

// `net ap|sta [ssid=..] [pass=..] [mdns=..]` — set every property of one network
// mode in a single command; omitted properties fall back to defaults (not the
// current value). `pass` is the AP password in ap mode, the Wi-Fi password in
// sta mode. Values cannot contain spaces (serial tokenizer limitation). Takes
// effect on the next restart. Returns true if the config changed.
static bool handleNet(const char *modeArg) {
  if (!modeArg) {
    Serial.println(F("[net] usage: net ap [pass=..] [mdns=..] | net sta ssid=.. [pass=..] [mdns=..]"));
    return false;
  }
  NetMode nm;
  if (!parseNetMode(modeArg, nm)) { Serial.println(F("[net] error: net ap|sta ...")); return false; }

  // Start from defaults; each key=value token overrides one property.
  char ssid[33] = "";
  char pass[65] = "";
  bool passGiven = false;
  char mdns[33];  strlcpy(mdns, BRAND_MDNS_HOST, sizeof(mdns));

  for (char *tok = strtok(NULL, " \t"); tok; tok = strtok(NULL, " \t")) {
    char *eq = strchr(tok, '=');
    if (!eq) { Serial.print(F("[net] ignoring '")); Serial.print(tok); Serial.println(F("'")); continue; }
    *eq = 0;
    const char *key = tok, *val = eq + 1;
    if      (strcmp(key, "ssid") == 0) strlcpy(ssid, val, sizeof(ssid));
    else if (strcmp(key, "pass") == 0) { strlcpy(pass, val, sizeof(pass)); passGiven = true; }
    else if (strcmp(key, "mdns") == 0) strlcpy(mdns, val, sizeof(mdns));
    else { Serial.print(F("[net] unknown key: ")); Serial.println(key); }
  }

  if (nm == NetMode::AP) {
    const char *apPw = passGiven ? pass : BRAND_AP_PASSWORD;
    if (strlen(apPw) < 8) { Serial.println(F("[net] error: AP pass needs >= 8 chars")); return false; }
    config.network.mode = NetMode::AP;
    strlcpy(config.network.apPassword, apPw, sizeof(config.network.apPassword));
  } else {  // STA
    if (ssid[0] == '\0') { Serial.println(F("[net] error: net sta requires ssid=..")); return false; }
    config.network.mode = NetMode::STA;
    strlcpy(config.network.ssid, ssid, sizeof(config.network.ssid));
    strlcpy(config.network.password, passGiven ? pass : "", sizeof(config.network.password));
  }
  strlcpy(config.network.mdnsHost, mdns, sizeof(config.network.mdnsHost));

  configSave();
  Serial.println(F("[net] saved — restart to apply"));
  printConfig();
  return true;
}

// Returns true if a command line was handled (so the display can refresh).
static bool handleLine(char *line) {
  char *cmd = strtok(line, " \t");
  if (!cmd) return false;
  char *arg = strtok(NULL, " \t");

  if (strcmp(cmd, "show") == 0) { printConfig(); return true; }

  if (strcmp(cmd, "mode") == 0) {
    if (!arg) { Serial.println(F("[cfg] usage: mode manual|auto|artisan")); return false; }
    Mode m;
    if (!parseMode(arg, m)) { Serial.println(F("[cfg] error: mode manual|auto|artisan")); return false; }
    config.mode = m;
    configSave();
    printConfig();
    return true;
  }

  if (strcmp(cmd, "min") == 0 || strcmp(cmd, "max") == 0) {
    bool isMin = (cmd[1] == 'i');
    if (!arg) { Serial.println(F("[cfg] usage: min <c> | min -")); return false; }

    if (strcmp(arg, "-") == 0 || strcmp(arg, "off") == 0 || strcmp(arg, "clear") == 0) {
      if (isMin) config.automatic.temperature.minC = NAN; else config.automatic.temperature.maxC = NAN;
      configSave();
      printConfig();
      return true;
    }
    char *end;
    float v = strtod(arg, &end);
    if (end == arg) { Serial.println(F("[cfg] error: invalid number")); return false; }

    float nmin = isMin ? v : config.automatic.temperature.minC;
    float nmax = isMin ? config.automatic.temperature.maxC : v;
    if (!isnan(nmin) && !isnan(nmax) && nmin >= nmax) {
      Serial.println(F("[cfg] error: min must be < max"));
      return false;
    }
    if (isMin) config.automatic.temperature.minC = v; else config.automatic.temperature.maxC = v;
    configSave();
    printConfig();
    return true;
  }

  if (strcmp(cmd, "hardmax") == 0) {
    if (!arg) { Serial.println(F("[cfg] usage: hardmax <c> | hardmax -")); return false; }
    if (strcmp(arg, "-") == 0 || strcmp(arg, "off") == 0 || strcmp(arg, "clear") == 0) {
      config.safety.hardMaxC = NAN;
      configSave();
      printConfig();
      return true;
    }
    char *end;
    float v = strtod(arg, &end);
    if (end == arg) { Serial.println(F("[cfg] error: invalid number")); return false; }
    config.safety.hardMaxC = v;
    configSave();
    printConfig();
    return true;
  }

  if (strcmp(cmd, "net") == 0) return handleNet(arg);

  Serial.println(F("[cfg] commands: show | mode manual|auto|artisan | min <c> | max <c> | min - | max - | hardmax <c> | hardmax -"));
  Serial.println(F("[cfg]           net ap [pass=..] [mdns=..] | net sta ssid=.. [pass=..] [mdns=..]"));
  return false;
}

static bool pollSerial() {
  static char buf[160];   // room for `net sta ssid=.. pass=..` (SSID 32 + pass 63)
  static uint8_t len = 0;
  bool handled = false;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      buf[len] = 0;
      if (len) handled |= handleLine(buf);
      len = 0;
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    } else {
      len = 0;  // overflow -> drop line
    }
  }
  return handled;
}

// ---- Display ----
static void drawCentered(const char *s, int16_t y) {
  display.drawUTF8((128 - display.getUTF8Width(s)) / 2, y, s);
}

// Boot splash: brand + a status line reporting what startup is doing (e.g. the
// network it is connecting to). line2 is optional (a second, smaller line).
static void showSplash(const char *line1, const char *line2) {
  display.clearBuffer();
  display.setFont(u8g2_font_helvB08_tf);
  drawCentered(BRAND_NAME, line2 ? 22 : 26);
  display.setFont(u8g2_font_6x10_tf);
  if (line1) drawCentered(line1, line2 ? 42 : 46);
  if (line2) drawCentered(line2, 55);
  display.sendBuffer();
}

static void renderScreen() {
  display.clearBuffer();

  display.setFont(u8g2_font_helvB08_tf);
  drawCentered(BRAND_NAME, 10);
  display.setFont(u8g2_font_4x6_tf);
  display.drawUTF8(0, 6, modeName(config.mode));                    // control mode — left
  const char *cm = netActiveMode();                                 // connection mode — right
  display.drawUTF8(128 - display.getUTF8Width(cm), 6, cm);
  display.drawHLine(0, 13, 128);

  // BT is the hero reading; ET rides compact on the right (telemetry only).
  // Values are fixed-width (3 integer digits + 1 decimal, e.g. 017.5) so the row
  // never shifts as readings grow — a layout that fits every temperature.
  char v[8];
  char bt[20];
  fmtFixed(v, sizeof(v), lastBtC);
  snprintf(bt, sizeof(bt), "BT %s°C", v);
  display.setFont(u8g2_font_7x13_tf);
  display.drawUTF8(4, 30, bt);

  char et[16];
  fmtFixed(v, sizeof(v), lastEtC);
  snprintf(et, sizeof(et), "ET %s", v);
  display.setFont(u8g2_font_6x12_tf);   // reuse an already-linked font (no extra flash)
  display.drawUTF8(128 - display.getUTF8Width(et) - 2, 30, et);

  // Faixa (Automático only) sits directly under the sensors — same rule as the
  // home page. Hidden in other modes; the bottom status bar below stays put.
  if (config.mode == Mode::AUTO) {
    char sMin[8], sMax[8], mm[20];
    fmtFixed(sMin, sizeof(sMin), config.automatic.temperature.minC);
    fmtFixed(sMax, sizeof(sMax), config.automatic.temperature.maxC);
    // Small "Faixa BT" label + fixed-width values keeps the whole line on screen.
    display.setFont(u8g2_font_4x6_tf);
    display.drawUTF8(4, 44, "Faixa BT");
    int16_t vx = 4 + display.getUTF8Width("Faixa BT") + 4;
    snprintf(mm, sizeof(mm), "%s / %s", sMin, sMax);
    display.setFont(u8g2_font_6x12_tf);
    display.drawUTF8(vx, 44, mm);
  }

  // Status label is pinned to the bottom bar in every mode, so it never shifts
  // position with the mode. (Accent-free — the 7x13B font lacks accents; the
  // latch states say BOOT, the physical clear action, not the web wording.)
  const char *st;
  switch (state) {
    case State::IDLE:    st = "Parado";          break;
    case State::RUN:     st = "Aquecendo";       break;
    case State::HOLD:    st = "Temperatura OK";  break;
    case State::LOCKOUT: st = "Falha — BOOT";    break;
    case State::ESTOP:   st = "Emerg. — BOOT";   break;
    case State::FAULT:   st = "Falha sensor BT"; break;
    default:             st = "";                break;
  }
  display.setFont(u8g2_font_7x13B_tf);
  display.drawUTF8(4, 61, st);

  display.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(100);

  configBegin();   // mount LittleFS + load /config.json

  pinMode(PIN_MAX6675_SCK, OUTPUT);
  digitalWrite(PIN_MAX6675_SCK, LOW);
  pinMode(PIN_MAX6675_CS_BT, OUTPUT);
  digitalWrite(PIN_MAX6675_CS_BT, HIGH);
  pinMode(PIN_MAX6675_CS_ET, OUTPUT);
  digitalWrite(PIN_MAX6675_CS_ET, HIGH);
  pinMode(PIN_MAX6675_SO, INPUT);

  // Actuator: force OFF before anything else (fail-safe habit).
  pinMode(PIN_INV_ENABLE, OUTPUT);
  digitalWrite(PIN_INV_ENABLE, LOW);

  pinMode(PIN_START_STOP, INPUT_PULLDOWN);
  pinMode(PIN_FLAME_FAULT, INPUT_PULLDOWN);
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);

  // Splash reports what startup is about to do, so the wait is transparent.
  display.begin();
  if (config.network.mode == NetMode::STA && config.network.staProvisioned()) {
    showSplash("Conectando em", config.network.ssid);
  } else {
    showSplash("Ponto de acesso", nullptr);
  }

  // Network: bring up AP (permanent, default) or STA per config, with AP
  // fallback. Started after the INV_ENABLE fail-safe above (safety-first order).
  // The splash above stays visible while a STA connect blocks (up to ~60 s).
  netBegin();

  // Watchdog: subscribe this (control) loop to the ESP32 task WDT. The Arduino
  // core inits the TWDT at boot with panic=reboot and a 5 s timeout (sdkconfig
  // CONFIG_ESP_TASK_WDT_*). If the loop ever stalls (e.g. in RUN), the WDT reboots
  // — and setup() forces PIN_INV_ENABLE LOW first thing, so a stall closes the
  // burner (fail-safe, safety invariant §9.6). Subscribed AFTER netBegin() so the
  // blocking STA connect (up to 60 s) never counts against us; fed once per loop().
  esp_task_wdt_add(NULL);

  Serial.println();
  Serial.println(F("[torrador] boot ok — burner control (bench)"));
  Serial.println(F("[torrador] serial: show | mode manual|auto|artisan | min <c> | max <c> | min - | max - | hardmax <c> | hardmax -"));
  Serial.println(F("[torrador]         net ap [pass=..] [mdns=..] | net sta ssid=.. [pass=..] [mdns=..]"));
  printConfig();

  delay(300);
  lastBtC = max6675ReadCelsius(PIN_MAX6675_CS_BT);
  lastEtC = max6675ReadCelsius(PIN_MAX6675_CS_ET);
  lastReadMs = millis();
  renderScreen();
}

void loop() {
  uint32_t now = millis();
  bool dirty = false;

  // Feed the task watchdog: reaching here proves the loop is alive. A stall past
  // the WDT timeout reboots (→ enable forced LOW in setup()). See setup().
  esp_task_wdt_reset();

  // ---- Network ----
  // Captive-portal DNS pump + deferred post-save reboot. Non-blocking.
  netLoop();

  // ---- Inputs ----
  if (pollSerial()) dirty = true;

  bool startEdge = btnUpdate(btnStart, now);
  btnUpdate(btnFault, now);
  bool bootEdge = btnUpdate(btnBoot, now);
  bool faultActive = btnFault.level;

  // Web commands act exactly like the physical buttons (the loop owns safety).
  switch (netTakeCommand()) {
    case NetCommand::START_STOP:  startEdge = true; break;
    case NetCommand::CLEAR_LATCH: bootEdge  = true; break;
    default: break;
  }

  if (now - lastReadMs >= SENSOR_INTERVAL_MS) {
    lastReadMs = now;
    lastBtC = max6675ReadCelsius(PIN_MAX6675_CS_BT);
    lastEtC = max6675ReadCelsius(PIN_MAX6675_CS_ET);
    dirty = true;
  }
  bool sensorFault = isnan(lastBtC);   // control & fault track BT only; ET is telemetry

  // ---- Demand source, by mode ----
  //   MANUAL  : flame follows START directly (min/max band ignored)
  //   AUTO    : min/max hysteresis on BT (flame-direct if the band is unset)
  //   ARTISAN : owned by Artisan over MODBUS (Phase 3) — no source yet
  bool band = (config.mode == Mode::AUTO) && config.automatic.temperature.configured();
  if (config.mode == Mode::ARTISAN) {
    demand = false;                                            // Phase 3: Artisan will drive this
  } else if (!band) {
    demand = true;                                            // MANUAL, or AUTO without a band
  } else if (!sensorFault) {
    if (lastBtC <= config.automatic.temperature.minC)      demand = true;
    else if (lastBtC >= config.automatic.temperature.maxC) demand = false;
    // between the thresholds: keep previous demand (hysteresis)
  }

  State prev = state;

  // ---- Global pre-empts ----
  // A mode change cuts the flame in every mode: switching control authority must
  // not inherit a running burner. Latched safety states keep their latch.
  static Mode lastMode = config.mode;
  if (config.mode != lastMode) {
    lastMode = config.mode;
    processOn = false;
    if (state != State::LOCKOUT && state != State::ESTOP) state = State::IDLE;
    Serial.println(F("[torrador] mode changed — burner off"));
  }
  // BOOT clears a latched safety stop (INV-fault lockout or Artisan e-stop).
  if (bootEdge && (state == State::LOCKOUT || state == State::ESTOP)) {
    state = State::IDLE; processOn = false;
  }
  // Sensor fault fails safe, except while latched (LOCKOUT/ESTOP need BOOT).
  if (sensorFault && state != State::LOCKOUT && state != State::ESTOP) {
    state = State::FAULT; processOn = false;
  }
  // Independent over-temperature cutoff: a hard ceiling on BT that holds in every
  // mode (manual/auto/artisan), above and beyond the AUTO band — off always wins.
  // Latches LOCKOUT: over-temp is a serious event, so clearing needs a deliberate
  // BOOT press. A NaN BT (sensor fault) never trips this — the comparison is false.
  if (config.safety.configured() && !sensorFault &&
      lastBtC >= config.safety.hardMaxC &&
      state != State::LOCKOUT && state != State::ESTOP) {
    state = State::LOCKOUT; processOn = false;
    Serial.println(F("[torrador] HARD-MAX over-temperature — LOCKOUT"));
  }

  if (config.mode == Mode::ARTISAN) {
    // The button is a latched EMERGENCY STOP: cut the INV now, block re-enable
    // (logic or Artisan) until a BOOT short press releases it.
    if (startEdge && state != State::ESTOP) {
      state = State::ESTOP; processOn = false;
      Serial.println(F("[torrador] EMERGENCY STOP"));
    }
  } else {
    // MANUAL / AUTO: the button toggles the process on/off.
    if (startEdge && state != State::LOCKOUT && state != State::FAULT) {
      processOn = !processOn;
      Serial.print(F("[torrador] process "));
      Serial.println(processOn ? F("START") : F("STOP"));
      if (!processOn) state = State::IDLE;
    }
  }

  // ---- State machine (optimistic: enable => firing; a fault drops to LOCKOUT) ----
  switch (state) {
    case State::IDLE:
      if (processOn && !sensorFault) state = demand ? State::RUN : State::HOLD;
      break;
    case State::RUN:
      if (!processOn)       state = State::IDLE;
      else if (faultActive) state = State::LOCKOUT;   // INV reported a flame fault
      else if (!demand)     state = State::HOLD;       // temperature satisfied
      break;
    case State::HOLD:
      if (!processOn)  state = State::IDLE;
      else if (demand) state = State::RUN;
      break;
    case State::LOCKOUT:
      break;  // latched — only BOOT (handled above) clears it
    case State::ESTOP:
      break;  // latched emergency stop — only BOOT (handled above) clears it
    case State::FAULT:
      if (!sensorFault) state = State::IDLE;   // sensor recovered
      break;
  }

  // ---- Output ----
  digitalWrite(PIN_INV_ENABLE, (state == State::RUN) ? HIGH : LOW);

  if (state != prev) {
    Serial.print(F("[torrador] state -> "));
    Serial.println(stateName(state));
    dirty = true;
  }

  // Publish live status for the web home dashboard (mirrors the OLED).
  AppStatus st;
  strlcpy(st.state, stateName(state), sizeof(st.state));
  st.btC       = lastBtC;
  st.etC       = lastEtC;
  st.processOn = processOn;
  netPublishStatus(st);

  if (dirty) renderScreen();
}
