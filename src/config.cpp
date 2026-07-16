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

const char *netModeName(NetMode m) {
  switch (m) {
    case NetMode::AP:  return "ap";
    case NetMode::STA: return "sta";
  }
  return "ap";
}

bool parseNetMode(const char *s, NetMode &out) {
  if (strcmp(s, "ap") == 0)  { out = NetMode::AP;  return true; }
  if (strcmp(s, "sta") == 0) { out = NetMode::STA; return true; }
  return false;
}

// --- Serialization (one block per group; extend as groups are added) ---
static void toJson(JsonDocument &doc) {
  doc["mode"] = modeName(config.mode);
  // automatic mode (BT band)
  JsonObject autom = doc["auto"].to<JsonObject>();
  JsonObject t = autom["temperature"].to<JsonObject>();
  if (!isnan(config.automatic.temperature.minC)) t["min_c"] = config.automatic.temperature.minC;
  if (!isnan(config.automatic.temperature.maxC)) t["max_c"] = config.automatic.temperature.maxC;
  // (unset values are omitted; they read back as NAN)
  // artisan mode: no fields yet
  // network provisioning
  JsonObject net = doc["network"].to<JsonObject>();
  net["mode"]        = netModeName(config.network.mode);
  net["ssid"]        = config.network.ssid;
  net["password"]    = config.network.password;
  net["ap_password"] = config.network.apPassword;
  net["mdns_host"]   = config.network.mdnsHost;
}

static void fromJson(JsonDocument &doc) {
  Mode m;
  config.mode = parseMode(doc["mode"] | "", m) ? m : Mode::MANUAL;

  JsonVariant minC = doc["auto"]["temperature"]["min_c"];
  JsonVariant maxC = doc["auto"]["temperature"]["max_c"];
  config.automatic.temperature.minC = minC.isNull() ? NAN : minC.as<float>();
  config.automatic.temperature.maxC = maxC.isNull() ? NAN : maxC.as<float>();

  NetMode nm;
  config.network.mode = parseNetMode(doc["network"]["mode"] | "", nm) ? nm : NetMode::AP;
  strlcpy(config.network.ssid,       doc["network"]["ssid"]        | "",                sizeof(config.network.ssid));
  strlcpy(config.network.password,   doc["network"]["password"]    | "",                sizeof(config.network.password));
  strlcpy(config.network.apPassword, doc["network"]["ap_password"] | BRAND_AP_PASSWORD, sizeof(config.network.apPassword));
  strlcpy(config.network.mdnsHost,   doc["network"]["mdns_host"]   | BRAND_MDNS_HOST,   sizeof(config.network.mdnsHost));
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
