#include "config.h"

#include <LittleFS.h>
#include <ArduinoJson.h>

Config config;

static const char *CONFIG_PATH = "/config.json";

const char *modeName(Mode m) {
  switch (m) {
    case Mode::MANUAL:  return "manual";
    case Mode::AUTO:    return "auto";
    case Mode::ARTISAN: return "artisan";
  }
  return "manual";
}

bool parseMode(const char *s, Mode &out) {
  if (strcmp(s, "manual") == 0)  { out = Mode::MANUAL;  return true; }
  if (strcmp(s, "auto") == 0)    { out = Mode::AUTO;    return true; }
  if (strcmp(s, "artisan") == 0) { out = Mode::ARTISAN; return true; }
  return false;
}

// --- Serialization (one block per group; extend as groups are added) ---
static void toJson(JsonDocument &doc) {
  doc["mode"] = modeName(config.mode);
  // manual mode
  JsonObject manual = doc["manual"].to<JsonObject>();
  JsonObject t = manual["temperature"].to<JsonObject>();
  if (!isnan(config.manual.temperature.minC)) t["min_c"] = config.manual.temperature.minC;
  if (!isnan(config.manual.temperature.maxC)) t["max_c"] = config.manual.temperature.maxC;
  // (unset values are omitted; they read back as NAN)
  // artisan mode: no fields yet
}

static void fromJson(JsonDocument &doc) {
  Mode m;
  config.mode = parseMode(doc["mode"] | "", m) ? m : Mode::MANUAL;

  JsonVariant minC = doc["manual"]["temperature"]["min_c"];
  JsonVariant maxC = doc["manual"]["temperature"]["max_c"];
  config.manual.temperature.minC = minC.isNull() ? NAN : minC.as<float>();
  config.manual.temperature.maxC = maxC.isNull() ? NAN : maxC.as<float>();
}

void configReset() {
  config = Config();
}

static bool configLoad() {
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.print(F("[config] parse error: "));
    Serial.println(err.c_str());
    return false;
  }
  fromJson(doc);
  return true;
}

bool configSave() {
  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) {
    Serial.println(F("[config] save: open failed"));
    return false;
  }
  JsonDocument doc;
  toJson(doc);
  bool ok = serializeJson(doc, f) > 0;
  f.close();
  if (!ok) Serial.println(F("[config] save: write failed"));
  return ok;
}

void configBegin() {
  if (!LittleFS.begin(true)) {   // format on first-time mount failure
    Serial.println(F("[config] LittleFS mount failed"));
    return;
  }
  if (configLoad()) {
    Serial.println(F("[config] loaded /config.json"));
  } else {
    configReset();   // absent or unreadable in the current format => start fresh
    Serial.println(F("[config] no/unreadable config; using defaults"));
  }
}
