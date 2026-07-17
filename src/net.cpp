#include "net.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include "board_config.h"   // WiFi / ESPmDNS / ESPAsyncWebServer / DNSServer includes
#include "branding.h"       // BRAND_NAME, BRAND_AP_SSID
#include "config.h"         // config.network.*
#include "diag.h"           // resource/health snapshot folded into /status

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

const char *netActiveMode() { return apActive ? "AP" : "STA"; }

// --- Access control -----------------------------------------------------------

// Optional HTTP Basic gate for the whole UI. When no admin password is set the
// interface stays open (trusted-network case). When one is set, every route
// below requires it; on failure we answer 401 so the browser prompts for it.
// Returns true when the request may proceed. Read live so enabling/disabling or
// changing the password takes effect immediately, without a reboot.
static bool requireAuth(AsyncWebServerRequest *req) {
  if (!config.network.adminAuthEnabled()) return true;
  if (req->authenticate(config.network.adminUser, config.network.adminPassword)) return true;
  req->requestAuthentication(AsyncAuthType::AUTH_BASIC, BRAND_NAME);
  return false;
}

// --- Static assets ------------------------------------------------------------

// Serve a LittleFS file with an explicit Cache-Control header. The manual
// server.on routes bypass ESPAsyncWebServer's serveStatic caching, so we attach
// the header ourselves. CSS is versioned via a ?v= query on its <link>, so it
// can carry a far-future immutable cache; HTML uses no-cache to stay fresh.
static void sendFileCached(AsyncWebServerRequest *req, const char *path,
                           const char *type, const char *cache) {
  AsyncWebServerResponse *res = req->beginResponse(LittleFS, path, type);
  res->addHeader("Cache-Control", cache);
  req->send(res);
}

// --- Route handlers -----------------------------------------------------------

// Brand name for any page (never hardcoded — white-label rule).
static void handleBrand(AsyncWebServerRequest *req) {
  if (!requireAuth(req)) return;
  AsyncResponseStream *res = req->beginResponseStream("application/json");
  JsonDocument doc;
  doc["name"] = BRAND_NAME;
  serializeJson(doc, *res);
  req->send(res);
}

// Full settings bootstrap: brand + operation + network, so the settings page can
// prefill every field. AP password is returned too — acceptable in V0 (local UI
// over the device's own link).
static void handleConfig(AsyncWebServerRequest *req) {
  if (!requireAuth(req)) return;
  AsyncResponseStream *res = req->beginResponseStream("application/json");
  JsonDocument doc;
  doc["name"] = BRAND_NAME;

  JsonObject op = doc["operation"].to<JsonObject>();
  op["mode"] = modeName(config.mode);
  float mn = config.automatic.temperature.minC, mx = config.automatic.temperature.maxC;
  if (isnan(mn)) op["min_c"] = nullptr; else op["min_c"] = mn;
  if (isnan(mx)) op["max_c"] = nullptr; else op["max_c"] = mx;
  // Independent over-temperature cutoff — applies in every mode.
  float hm = config.safety.hardMaxC;
  if (isnan(hm)) op["hard_max_c"] = nullptr; else op["hard_max_c"] = hm;

  JsonObject nw = doc["network"].to<JsonObject>();
  nw["mode"]         = netModeName(config.network.mode);
  nw["ssid"]         = config.network.ssid;
  nw["ap_ssid"]      = BRAND_AP_SSID;
  nw["ap_password"]  = config.network.apPassword;
  nw["mdns"]         = config.network.mdnsHost;
  // Never expose the STA password; just say whether one is stored, so the UI can
  // show a masked placeholder and let the user keep it.
  nw["has_password"] = (config.network.password[0] != '\0');
  // UI login: the username is not a secret (the operator must type it), so it is
  // returned; the password is only reported as set/unset.
  nw["admin_user"] = config.network.adminUser;
  nw["has_admin_password"] = config.network.adminAuthEnabled();

  serializeJson(doc, *res);
  req->send(res);
}

// Save operation config (control mode + BT band). Applies live — the control
// loop reads config each iteration — so no reboot. Empty min/max clears the band.
// The over-temp cutoff (hard_max) lives under /security, not here.
static void handleOperation(AsyncWebServerRequest *req) {
  if (!requireAuth(req)) return;
  auto reject = [&](const char *why) {
    AsyncResponseStream *r = req->beginResponseStream("application/json");
    r->setCode(400);
    JsonDocument d; d["ok"] = false; d["error"] = why;
    serializeJson(d, *r); req->send(r);
  };

  // Validate everything into locals first; only mutate config once it all passes.
  Mode desired = config.mode;
  if (req->hasParam("mode", true)) {
    if (!parseMode(req->getParam("mode", true)->value().c_str(), desired)) { reject("mode"); return; }
  }

  float nmin = config.automatic.temperature.minC;
  float nmax = config.automatic.temperature.maxC;
  auto readTemp = [&](const char *name, float &dst) -> bool {
    if (!req->hasParam(name, true)) return true;   // absent => keep current
    String v = req->getParam(name, true)->value(); v.trim();
    if (!v.length() || v == "-") { dst = NAN; return true; }   // blank => clear
    char *end; double f = strtod(v.c_str(), &end);
    if (end == v.c_str()) return false;
    dst = (float)f; return true;
  };
  if (!readTemp("min", nmin)) { reject("min"); return; }
  if (!readTemp("max", nmax)) { reject("max"); return; }
  if (!isnan(nmin) && !isnan(nmax) && nmin >= nmax) { reject("range"); return; }
  // AUTO regulates on the band, so both limits are required.
  if (desired == Mode::AUTO && (isnan(nmin) || isnan(nmax))) { reject("band"); return; }

  config.mode = desired;
  config.automatic.temperature.minC = nmin;
  config.automatic.temperature.maxC = nmax;
  bool ok = configSave();
  Serial.print(F("[net] operation saved: mode=")); Serial.println(modeName(config.mode));

  AsyncResponseStream *res = req->beginResponseStream("application/json");
  JsonDocument doc; doc["ok"] = ok;
  serializeJson(doc, *res); req->send(res);
}

// Save security config: the over-temp cutoff (hard_max) + the optional UI
// password. Applies live (configSave, no reboot). Blank hard_max disables it.
// Password follows the "keep saved secret" convention: field absent => keep the
// stored one (masked in the UI); present but empty => disable auth; present and
// non-empty => set a new password.
static void handleSecurity(AsyncWebServerRequest *req) {
  if (!requireAuth(req)) return;
  auto reject = [&](const char *why) {
    AsyncResponseStream *r = req->beginResponseStream("application/json");
    r->setCode(400);
    JsonDocument d; d["ok"] = false; d["error"] = why;
    serializeJson(d, *r); req->send(r);
  };

  // Over-temperature cutoff (all modes). Blank => disabled. Validate into a local.
  float nhard = config.safety.hardMaxC;
  if (req->hasParam("hard_max", true)) {
    String v = req->getParam("hard_max", true)->value(); v.trim();
    if (!v.length() || v == "-") {
      nhard = NAN;
    } else {
      char *end; double f = strtod(v.c_str(), &end);
      if (end == v.c_str()) { reject("hard_max"); return; }
      nhard = (float)f;
    }
  }

  config.safety.hardMaxC = nhard;
  // Username: always submitted (not a secret). Blank falls back to the default so
  // the login never becomes impossible (empty user + a set password).
  if (req->hasParam("admin_user", true)) {
    String u = req->getParam("admin_user", true)->value(); u.trim();
    strlcpy(config.network.adminUser, u.length() ? u.c_str() : BRAND_ADMIN_USER,
            sizeof(config.network.adminUser));
  }
  // Password follows the "keep saved secret" convention (see the comment above).
  if (req->hasParam("admin_password", true)) {
    strlcpy(config.network.adminPassword, req->getParam("admin_password", true)->value().c_str(),
            sizeof(config.network.adminPassword));
  }
  bool ok = configSave();
  Serial.print(F("[net] security saved: auth="));
  Serial.println(config.network.adminAuthEnabled() ? F("on") : F("off"));

  AsyncResponseStream *res = req->beginResponseStream("application/json");
  JsonDocument doc; doc["ok"] = ok;
  serializeJson(doc, *res); req->send(res);
}

// Visible Wi-Fi networks. Non-blocking: a synchronous scan would stall the
// AsyncTCP task and hang the request, so we kick off an async scan and let the
// client poll. Response: {"scanning": bool, "networks": [{ssid, rssi, enc}]}.
static void handleScan(AsyncWebServerRequest *req) {
  if (!requireAuth(req)) return;
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
  if (!requireAuth(req)) return;
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
  strlcpy(config.network.mdnsHost,   param("mdns", BRAND_MDNS_HOST).c_str(), sizeof(config.network.mdnsHost));
  // STA password: update only when the field was actually submitted. A disabled
  // (masked) field is omitted from the form, which means "keep the saved one".
  if (req->hasParam("password", true)) {
    strlcpy(config.network.password, req->getParam("password", true)->value().c_str(),
            sizeof(config.network.password));
  }

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
  if (!requireAuth(req)) return;
  AsyncResponseStream *res = req->beginResponseStream("application/json");
  JsonDocument doc;
  doc["mode"]       = modeName(config.mode);
  doc["net"]        = netActiveMode();       // "AP" / "STA" (actual interface)
  // Network name shown on the dashboard: the AP's own SSID, or the joined network.
  doc["ssid"]       = apActive ? BRAND_AP_SSID : config.network.ssid;
  doc["state"]      = pubStatus.state;
  doc["process_on"] = pubStatus.processOn;
  if (isnan(pubStatus.btC)) doc["bt_c"] = nullptr; else doc["bt_c"] = pubStatus.btC;
  if (isnan(pubStatus.etC)) doc["et_c"] = nullptr; else doc["et_c"] = pubStatus.etC;

  float mn = config.automatic.temperature.minC;
  float mx = config.automatic.temperature.maxC;
  if (isnan(mn)) doc["min_c"] = nullptr; else doc["min_c"] = mn;
  if (isnan(mx)) doc["max_c"] = nullptr; else doc["max_c"] = mx;

  // Resource/health monitor (discreet footer on the dashboard). Cheap snapshot,
  // already refreshed at 1 Hz by the control loop — no extra work per request.
  const DiagMetrics &m = diagSnapshot();
  JsonObject diag  = doc["diag"].to<JsonObject>();
  diag["free_heap"] = m.freeHeap;
  diag["min_heap"]  = m.minFreeHeap;
  diag["heap_size"] = m.heapSize;
  diag["lps"]       = m.lps;
  diag["uptime_s"]  = m.uptimeS;
  diag["rssi"]      = m.rssi;

  serializeJson(doc, *res);
  req->send(res);
}

// Web control commands — queued for the control loop (single slot). START/STOP
// and latch-clear mirror the physical buttons; the loop owns all safety logic.
static void handleCommand(AsyncWebServerRequest *req) {
  if (!requireAuth(req)) return;
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
    if (!requireAuth(req)) return;
    sendFileCached(req, "/index.html", "text/html", "no-cache");   // home dashboard
  });
  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;
    sendFileCached(req, "/portal.html", "text/html", "no-cache");  // settings (operation + network + security)
  });
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;                                 // shared UI system — versioned via ?v=
    sendFileCached(req, "/style.css", "text/css", "public, max-age=31536000, immutable");
  });
  server.on("/brand",     HTTP_GET,  handleBrand);
  server.on("/config",    HTTP_GET,  handleConfig);
  server.on("/status",    HTTP_GET,  handleStatus);
  server.on("/command",   HTTP_POST, handleCommand);
  server.on("/operation", HTTP_POST, handleOperation);
  server.on("/security",  HTTP_POST, handleSecurity);
  server.on("/scan",      HTTP_GET,  handleScan);
  server.on("/save",      HTTP_POST, handleSave);
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
