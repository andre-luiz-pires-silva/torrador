#include "net.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include "board_config.h"   // WiFi / ESPmDNS / ESPAsyncWebServer / DNSServer includes
#include "branding.h"       // BRAND_NAME, BRAND_AP_SSID
#include "config.h"         // config.network.*

// Configuration-mode network: fixed AP address + captive portal.
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_NETMASK(255, 255, 255, 0);
static const byte      DNS_PORT = 53;

static AsyncWebServer server(80);
static DNSServer      dnsServer;

static bool     apActive   = false;   // true while the soft-AP + captive DNS are up
// Deferred reboot after /save so the HTTP response flushes before we restart.
static uint32_t rebootAtMs = 0;

// Home-dashboard bridge (see net.h).
static AppStatus           pubStatus;
static volatile NetCommand pendingCmd = NetCommand::NONE;

void netPublishStatus(const AppStatus &s) { pubStatus = s; }

NetCommand netTakeCommand() {
  NetCommand c = pendingCmd;
  pendingCmd = NetCommand::NONE;
  return c;
}

// --- Route handlers -----------------------------------------------------------

// Bootstrap the portal: brand name (never hardcoded — white-label rule) plus the
// persisted network config so the form reflects what's saved. AP password is
// returned too — acceptable in V0 (local UI over the device's own link).
static void handleBrand(AsyncWebServerRequest *req) {
  AsyncResponseStream *res = req->beginResponseStream("application/json");
  JsonDocument doc;
  doc["name"]        = BRAND_NAME;
  doc["ap_ssid"]     = BRAND_AP_SSID;
  doc["mode"]        = netModeName(config.network.mode);
  doc["ssid"]        = config.network.ssid;        // STA target ("" if none)
  doc["ap_password"] = config.network.apPassword;
  doc["mdns"]        = config.network.mdnsHost;
  serializeJson(doc, *res);
  req->send(res);
}

// Visible Wi-Fi networks. Non-blocking: a synchronous scan would stall the
// AsyncTCP task and hang the request, so we kick off an async scan and let the
// client poll. Response: {"scanning": bool, "networks": [{ssid, rssi, enc}]}.
static void handleScan(AsyncWebServerRequest *req) {
  int n = WiFi.scanComplete();

  if (n == WIFI_SCAN_FAILED) {          // -2: idle -> start an async scan
    WiFi.scanNetworks(true /*async*/, true /*show hidden*/);
    Serial.println(F("[net] scan started"));
    AsyncResponseStream *res = req->beginResponseStream("application/json");
    JsonDocument doc;
    doc["scanning"] = true;
    doc["networks"].to<JsonArray>();
    serializeJson(doc, *res);
    req->send(res);
    return;
  }

  if (n == WIFI_SCAN_RUNNING) {         // -1: still in progress
    AsyncResponseStream *res = req->beginResponseStream("application/json");
    JsonDocument doc;
    doc["scanning"] = true;
    doc["networks"].to<JsonArray>();
    serializeJson(doc, *res);
    req->send(res);
    return;
  }

  Serial.print(F("[net] scan done: "));
  Serial.print(n);
  Serial.println(F(" network(s)"));
  AsyncResponseStream *res = req->beginResponseStream("application/json");
  JsonDocument doc;
  doc["scanning"] = false;
  JsonArray arr = doc["networks"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["enc"]  = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
  }
  serializeJson(doc, *res);
  req->send(res);
  WiFi.scanDelete();                    // free results; next call starts fresh
}

// Persist network mode + credentials, then reboot to apply (connect STA, or
// restart the AP with the new password). Rejects an AP password shorter than 8
// (WPA2 minimum) without saving.
static void handleSave(AsyncWebServerRequest *req) {
  auto param = [&](const char *name, const char *fallback) -> String {
    if (req->hasParam(name, true)) {
      const String &v = req->getParam(name, true)->value();
      return v.length() ? v : String(fallback);
    }
    return String(fallback);
  };

  String modeStr = param("mode", netModeName(config.network.mode));
  String apPw    = param("ap_password", config.network.apPassword);

  auto reject = [&](const char *why) {
    AsyncResponseStream *res = req->beginResponseStream("application/json");
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = why;
    serializeJson(doc, *res);
    req->send(res);
  };

  NetMode nm;
  if (!parseNetMode(modeStr.c_str(), nm)) { reject("mode"); return; }
  if (apPw.length() < 8)                  { reject("ap_pw"); return; }

  config.network.mode = nm;
  strlcpy(config.network.apPassword, apPw.c_str(),                 sizeof(config.network.apPassword));
  strlcpy(config.network.ssid,       param("ssid", "").c_str(),     sizeof(config.network.ssid));
  strlcpy(config.network.password,   param("password", "").c_str(), sizeof(config.network.password));
  strlcpy(config.network.mdnsHost,   param("mdns", BRAND_MDNS_HOST).c_str(), sizeof(config.network.mdnsHost));

  bool ok = configSave();
  Serial.print(F("[net] saved: mode="));
  Serial.print(netModeName(config.network.mode));
  Serial.print(F(" ssid='"));
  Serial.print(config.network.ssid);
  Serial.println(ok ? F("'") : F("' — SAVE FAILED"));

  AsyncResponseStream *res = req->beginResponseStream("application/json");
  JsonDocument doc;
  doc["ok"]   = ok;
  doc["mode"] = netModeName(config.network.mode);
  doc["ssid"] = config.network.ssid;
  serializeJson(doc, *res);
  req->send(res);

  if (ok) rebootAtMs = millis() + 1200;   // apply on reboot; let the response flush first
}

// Live controller status for the home dashboard (mirrors the OLED display).
static void handleStatus(AsyncWebServerRequest *req) {
  AsyncResponseStream *res = req->beginResponseStream("application/json");
  JsonDocument doc;
  doc["mode"]       = modeName(config.mode);
  doc["state"]      = pubStatus.state;
  doc["process_on"] = pubStatus.processOn;
  if (isnan(pubStatus.tempC)) doc["temp_c"] = nullptr; else doc["temp_c"] = pubStatus.tempC;

  float mn = config.manual.temperature.minC;
  float mx = config.manual.temperature.maxC;
  if (isnan(mn)) doc["min_c"] = nullptr; else doc["min_c"] = mn;
  if (isnan(mx)) doc["max_c"] = nullptr; else doc["max_c"] = mx;

  serializeJson(doc, *res);
  req->send(res);
}

// Web control commands — queued for the control loop (single slot). START/STOP
// and latch-clear mirror the physical buttons; the loop owns all safety logic.
static void handleCommand(AsyncWebServerRequest *req) {
  String cmd = req->hasParam("cmd", true) ? req->getParam("cmd", true)->value() : "";
  bool ok = true;
  if      (cmd == "startstop") pendingCmd = NetCommand::START_STOP;
  else if (cmd == "clear")     pendingCmd = NetCommand::CLEAR_LATCH;
  else                         ok = false;

  AsyncResponseStream *res = req->beginResponseStream("application/json");
  res->setCode(ok ? 200 : 400);
  JsonDocument doc;
  doc["ok"] = ok;
  serializeJson(doc, *res);
  req->send(res);
}

// Register web routes and start the server. Called in every mode so the UI is
// reachable over AP and STA alike.
static void setupRoutes() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(LittleFS, "/index.html", "text/html");     // home dashboard
  });
  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(LittleFS, "/portal.html", "text/html");    // network settings
  });
  server.on("/brand",   HTTP_GET,  handleBrand);
  server.on("/status",  HTTP_GET,  handleStatus);
  server.on("/command", HTTP_POST, handleCommand);
  server.on("/scan",    HTTP_GET,  handleScan);
  server.on("/save",    HTTP_POST, handleSave);
  // Anything else (incl. OS captive-portal probes) -> bounce to the portal.
  server.onNotFound([](AsyncWebServerRequest *req) {
    req->redirect("http://192.168.4.1/");
  });
  server.begin();
}

static void startMdns() {
  if (MDNS.begin(config.network.mdnsHost)) {
    MDNS.addService("http", "tcp", 80);
  }
}

// Permanent AP (or STA fallback): the device is the WPA2 access point. AP_STA
// mode keeps the station interface enabled so WiFi.scanNetworks() still works.
static void netStartAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK);
  bool apOk = WiFi.softAP(BRAND_AP_SSID, config.network.apPassword);

  // Captive portal: answer every DNS query with our AP IP so the phone's
  // connectivity check redirects into the page automatically.
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", AP_IP);
  apActive = true;
  startMdns();

  Serial.print(F("[net] AP mode — '"));
  Serial.print(BRAND_AP_SSID);
  Serial.print(apOk ? F("' up (WPA2)  ") : F("' FAILED  "));
  Serial.print(F("http://192.168.4.1  /  http://"));
  Serial.print(config.network.mdnsHost);
  Serial.println(F(".local"));
}

// Join the configured Wi-Fi. Blocks up to 60 s (safe in setup: nothing runs yet,
// INV is disabled, and waitForConnectResult feeds the watchdog). Returns true on
// a successful connection.
static bool netStartStation() {
  Serial.print(F("[net] STA — connecting to '"));
  Serial.print(config.network.ssid);
  Serial.println(F("'…"));
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.network.ssid, config.network.password);

  if (WiFi.waitForConnectResult(60000) != WL_CONNECTED) {
    Serial.println(F("[net] STA — connect failed"));
    return false;
  }
  apActive = false;
  startMdns();
  Serial.print(F("[net] STA mode — connected: http://"));
  Serial.print(WiFi.localIP());
  Serial.print(F("  /  http://"));
  Serial.print(config.network.mdnsHost);
  Serial.println(F(".local"));
  return true;
}

// --- Public API ---------------------------------------------------------------

void netBegin() {
  // Bring up the Wi-Fi interface FIRST: WiFi.mode()/softAP() initialize the
  // lwIP/tcpip stack. Registering routes (server.begin() opens a listening
  // socket) before that asserts "Invalid mbox" and reboot-loops.
  //
  // STA only when explicitly chosen and a target SSID exists; otherwise (and on
  // any STA failure) run as the permanent AP so the device is never unreachable.
  bool sta = config.network.mode == NetMode::STA &&
             config.network.staProvisioned() &&
             netStartStation();
  if (!sta) netStartAP();

  setupRoutes();   // network stack is up now — safe to start the web server
}

void netLoop() {
  if (apActive) dnsServer.processNextRequest();

  if (rebootAtMs && (int32_t)(millis() - rebootAtMs) >= 0) {
    ESP.restart();
  }
}
