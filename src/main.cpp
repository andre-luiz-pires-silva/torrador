#include <Arduino.h>
#include <U8g2lib.h>
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
static float lastTempC = NAN;

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

static void printConfig() {
  Serial.print(F("[cfg] mode=")); Serial.println(modeName(config.mode));
  char sMin[8], sMax[8];
  fmtTemp(sMin, sizeof(sMin), config.manual.temperature.minC);
  fmtTemp(sMax, sizeof(sMax), config.manual.temperature.maxC);
  Serial.print(F("[cfg] min="));  Serial.print(sMin);
  Serial.print(F("  max="));      Serial.print(sMax);
  Serial.print(F("  -> "));
  Serial.println(config.manual.temperature.configured() ? F("min/max") : F("flame direct"));
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
      if (isMin) config.manual.temperature.minC = NAN; else config.manual.temperature.maxC = NAN;
      configSave();
      printConfig();
      return true;
    }
    char *end;
    float v = strtod(arg, &end);
    if (end == arg) { Serial.println(F("[cfg] error: invalid number")); return false; }

    float nmin = isMin ? v : config.manual.temperature.minC;
    float nmax = isMin ? config.manual.temperature.maxC : v;
    if (!isnan(nmin) && !isnan(nmax) && nmin >= nmax) {
      Serial.println(F("[cfg] error: min must be < max"));
      return false;
    }
    if (isMin) config.manual.temperature.minC = v; else config.manual.temperature.maxC = v;
    configSave();
    printConfig();
    return true;
  }

  Serial.println(F("[cfg] commands: show | mode manual|auto|artisan | min <c> | max <c> | min - | max -"));
  return false;
}

static bool pollSerial() {
  static char buf[48];
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

static void renderScreen() {
  display.clearBuffer();

  display.setFont(u8g2_font_helvB08_tf);
  drawCentered(BRAND_NAME, 10);
  display.setFont(u8g2_font_4x6_tf);
  const char *mn = modeName(config.mode);
  display.drawUTF8(128 - display.getUTF8Width(mn), 6, mn);
  display.drawHLine(0, 13, 128);

  display.setFont(u8g2_font_7x13_tf);
  char t[24];
  if (isnan(lastTempC)) snprintf(t, sizeof(t), "BT: -- °C");
  else                  snprintf(t, sizeof(t), "BT: %.1f °C", lastTempC);
  display.drawUTF8(4, 30, t);

  const char *st;
  switch (state) {
    case State::IDLE:    st = "Parado";       break;
    case State::RUN:     st = "Aquecendo";    break;
    case State::HOLD:    st = "Temp. OK";     break;
    case State::LOCKOUT: st = "FALHA — BOOT"; break;
    case State::ESTOP:   st = "EMERG. — BOOT"; break;
    case State::FAULT:   st = "Sensor BT!";   break;
    default:             st = "";             break;
  }
  display.setFont(u8g2_font_7x13B_tf);
  display.drawUTF8(4, 47, st);

  char sMin[8], sMax[8], mm[24];
  fmtTemp(sMin, sizeof(sMin), config.manual.temperature.minC);
  fmtTemp(sMax, sizeof(sMax), config.manual.temperature.maxC);
  snprintf(mm, sizeof(mm), "Min:%s  Max:%s", sMin, sMax);
  display.setFont(u8g2_font_6x12_tf);
  display.drawUTF8(4, 61, mm);

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
  pinMode(PIN_MAX6675_SO, INPUT);

  // Actuator: force OFF before anything else (fail-safe habit).
  pinMode(PIN_INV_ENABLE, OUTPUT);
  digitalWrite(PIN_INV_ENABLE, LOW);

  pinMode(PIN_START_STOP, INPUT_PULLDOWN);
  pinMode(PIN_FLAME_FAULT, INPUT_PULLDOWN);
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);

  display.begin();
  display.clearBuffer();
  display.setFont(u8g2_font_helvB08_tf);
  drawCentered(BRAND_NAME, 28);
  display.setFont(u8g2_font_6x10_tf);
  drawCentered("Controle de chama", 44);
  display.sendBuffer();

  // Network: bring up AP (permanent, default) or STA per config, with AP
  // fallback. Started after the INV_ENABLE fail-safe above (safety-first order).
  netBegin();

  Serial.println();
  Serial.println(F("[torrador] boot ok — burner control (bench)"));
  Serial.println(F("[torrador] serial: show | mode manual|auto|artisan | min <c> | max <c> | min - | max -"));
  printConfig();

  delay(300);
  lastTempC = max6675ReadCelsius(PIN_MAX6675_CS_BT);
  lastReadMs = millis();
  renderScreen();
}

void loop() {
  uint32_t now = millis();
  bool dirty = false;

  // ---- Network ----
  // Captive-portal DNS pump + deferred post-save reboot. Non-blocking.
  netLoop();

  // ---- Inputs ----
  if (pollSerial()) dirty = true;

  bool startEdge = btnUpdate(btnStart, now);
  btnUpdate(btnFault, now);
  bool bootEdge = btnUpdate(btnBoot, now);
  bool faultActive = btnFault.level;

  if (now - lastReadMs >= SENSOR_INTERVAL_MS) {
    lastReadMs = now;
    lastTempC = max6675ReadCelsius(PIN_MAX6675_CS_BT);
    dirty = true;
  }
  bool sensorFault = isnan(lastTempC);

  // ---- Demand source, by mode ----
  //   MANUAL  : flame follows START directly (min/max band ignored)
  //   AUTO    : min/max hysteresis on BT (flame-direct if the band is unset)
  //   ARTISAN : owned by Artisan over MODBUS (Phase 3) — no source yet
  bool band = (config.mode == Mode::AUTO) && config.manual.temperature.configured();
  if (config.mode == Mode::ARTISAN) {
    demand = false;                                            // Phase 3: Artisan will drive this
  } else if (!band) {
    demand = true;                                            // MANUAL, or AUTO without a band
  } else if (!sensorFault) {
    if (lastTempC <= config.manual.temperature.minC)      demand = true;
    else if (lastTempC >= config.manual.temperature.maxC) demand = false;
    // between the thresholds: keep previous demand (hysteresis)
  }

  State prev = state;

  // ---- Global pre-empts ----
  // BOOT clears a latched safety stop (INV-fault lockout or Artisan e-stop).
  if (bootEdge && (state == State::LOCKOUT || state == State::ESTOP)) {
    state = State::IDLE; processOn = false;
  }
  // Sensor fault fails safe, except while latched (LOCKOUT/ESTOP need BOOT).
  if (sensorFault && state != State::LOCKOUT && state != State::ESTOP) {
    state = State::FAULT; processOn = false;
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
  if (dirty) renderScreen();
}
