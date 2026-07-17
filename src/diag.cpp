#include "diag.h"

#include <WiFi.h>
#include <esp_system.h>

// -----------------------------------------------------------------------------
// diag.cpp — see diag.h. All state is file-local; the snapshot is the only value
// shared out (by const ref), refreshed at 1 Hz from the control loop.
// -----------------------------------------------------------------------------

static DiagMetrics g_metrics;
static uint32_t g_loopCount   = 0;   // loops since the last sample
static uint32_t g_lastSampleMs = 0;

static const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "ext";
    case ESP_RST_SW:        return "sw";
    case ESP_RST_PANIC:     return "panic";      // crash — worth noticing
    case ESP_RST_INT_WDT:   return "int-wdt";
    case ESP_RST_TASK_WDT:  return "task-wdt";   // the loop stalled past the WDT
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_BROWNOUT:  return "brownout";   // power-supply health signal
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    default:                return "unknown";
  }
}

void diagBegin() {
  g_lastSampleMs = millis();
  // Prime the snapshot so a /status poll before the first 1 s tick isn't zeroed.
  g_metrics.freeHeap    = ESP.getFreeHeap();
  g_metrics.minFreeHeap = ESP.getMinFreeHeap();
  g_metrics.heapSize    = ESP.getHeapSize();

  Serial.print(F("[diag] reset="));
  Serial.print(resetReasonName(esp_reset_reason()));
  Serial.print(F("  heap="));  Serial.print(g_metrics.freeHeap);
  Serial.print(F("/"));        Serial.print(g_metrics.heapSize);
  Serial.print(F("  min="));   Serial.println(g_metrics.minFreeHeap);
}

void diagLoop(uint32_t now) {
  g_loopCount++;
  uint32_t elapsed = now - g_lastSampleMs;
  if (elapsed < 1000) return;
  g_lastSampleMs = now;

  g_metrics.lps         = (uint32_t)((uint64_t)g_loopCount * 1000 / elapsed);
  g_loopCount           = 0;
  g_metrics.freeHeap    = ESP.getFreeHeap();
  g_metrics.minFreeHeap = ESP.getMinFreeHeap();
  g_metrics.heapSize    = ESP.getHeapSize();
  g_metrics.uptimeS     = now / 1000;
  g_metrics.rssi        = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
}

const DiagMetrics &diagSnapshot() { return g_metrics; }

void diagPrint() {
  const DiagMetrics &m = g_metrics;
  Serial.print(F("[diag] up="));  Serial.print(m.uptimeS);  Serial.print(F("s"));
  Serial.print(F("  heap="));     Serial.print(m.freeHeap);
  Serial.print(F("/"));           Serial.print(m.heapSize);
  Serial.print(F("  min="));      Serial.print(m.minFreeHeap);
  Serial.print(F("  lps="));      Serial.print(m.lps);
  Serial.print(F("  rssi="));     Serial.print(m.rssi);  Serial.println(F("dBm"));
}
