#pragma once

#include <Arduino.h>

// -----------------------------------------------------------------------------
// diag.h — lightweight runtime resource & health monitor (V0).
//
// Purpose: give early warning when a feature starts costing too much RAM or CPU,
// and surface basic equipment health (reset cause, link quality, uptime).
//
// Design constraint: the monitor must NOT worsen the pressure it watches. Every
// metric is an O(1) SDK read (heap counters, millis, RSSI) plus one loop counter,
// so collection is essentially free. State is a single fixed struct (no String,
// no allocation), sampled once per second from the cooperative loop. Surfaced on
// the serial `stat` command and folded into the /status JSON the UI already polls
// at 1 Hz — no extra request, no extra timer, no FreeRTOS task.
// -----------------------------------------------------------------------------

struct DiagMetrics {
  uint32_t freeHeap    = 0;   // bytes free now (ESP.getFreeHeap)
  uint32_t minFreeHeap = 0;   // low-water mark since boot — catches transient spikes
  uint32_t heapSize    = 0;   // total heap, for a % figure
  uint32_t lps         = 0;   // loop iterations in the last sampled second (CPU headroom proxy)
  uint32_t uptimeS     = 0;   // seconds since boot
  int32_t  rssi        = 0;   // Wi-Fi RSSI dBm in STA (0 when not associated / AP mode)
};

// Log boot-time heap and the reset reason. Call once at the end of setup().
void diagBegin();

// Count one loop and, once per second, refresh the snapshot. Call every loop()
// iteration (the per-call cost is a single increment).
void diagLoop(uint32_t now);

// Latest 1 Hz snapshot. Safe to read from the async web task: scalar fields, and
// a torn read is harmless for a monitor.
const DiagMetrics &diagSnapshot();

// One-line human summary to Serial (the `stat` command).
void diagPrint();
