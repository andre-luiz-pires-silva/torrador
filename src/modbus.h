#pragma once

#include <Arduino.h>   // uint16_t — keep this header self-contained

// -----------------------------------------------------------------------------
// modbus.h — MODBUS TCP slave for the Artisan software (PRD §4.1, Phase 2).
//
// The ESP is the MODBUS TCP server/slave; Artisan is the client/master. The
// register map (fixed by the PRD):
//
//   Input Register  0 (read)  — BT (°C × 10)
//   Input Register  1 (read)  — ET (°C × 10)
//   Holding Register 0 (write) — burner power 0..100
//
// The server rides the same AsyncTCP stack as the web server, so there is no
// extra FreeRTOS task and nothing to pump from loop() — it is event-driven.
// It listens on config.artisan.port in every mode (Artisan may log temperatures
// even in manual/auto); the control loop only acts on the burner-power register
// while config.mode == ARTISAN.
// -----------------------------------------------------------------------------

// --- Control loop <-> MODBUS bridge -----------------------------------------
// Same shape as the net.h dashboard bridge: the MODBUS workers run on the
// AsyncTCP task, the control loop on loop(). These calls pass telemetry out and
// the burner-power command in. Registers are single 16-bit words (atomic on the
// 32-bit ESP32), so no lock is needed — good enough for V0.

// Register the MODBUS workers. Call once in setup(), after configBegin() and
// netBegin(). Does not open the port — modbusSetActive() controls listening.
void modbusBegin();

// Open (active) or close the MODBUS TCP slave. Artisan can connect only while it
// is open, so call with (config.mode == ARTISAN): the port listens in Artisan
// mode and is closed (dropping any connection) in manual/auto. Idempotent — only
// acts on an actual change.
void modbusSetActive(bool active);

// Publish the latest temperatures into the input registers (BT=IR0, ET=IR1) as
// °C×10. NaN (sensor fault) is published as 0. Call from the control loop.
void modbusPublishTemps(float btC, float etC);

// Latest burner power (0..100) written by Artisan into holding register 0.
// Meaningful only in ARTISAN mode; the caller maps it to on/off by threshold.
uint16_t modbusBurnerPower();

// True if Artisan sent any MODBUS request within the last `withinMs`. Activity-
// based (survives the server's idle-timeout socket drops), so it reflects real
// polling — the meaningful "Artisan is talking to us" signal, not just an open
// socket. False until the first request is ever received.
bool modbusLinked(uint32_t withinMs);
