#include "modbus.h"

#include "ModbusServerTCPasync.h"   // eModbus — async TCP server over AsyncTCP

#include "config.h"

// -----------------------------------------------------------------------------
// MODBUS TCP slave (Artisan). See modbus.h for the register map and rationale.
//
// eModbus keeps no register storage of its own: we hold the registers here and
// answer each function code in a worker. Workers run on the AsyncTCP task; the
// control loop reads/writes the same words via the bridge in modbus.h. Single
// 16-bit accesses are atomic on the ESP32, matching the net.cpp bridge style.
// -----------------------------------------------------------------------------

static ModbusServerTCPasync mbServer;

// Artisan's MODBUS "slave/unit ID". Artisan must be configured with the same id.
static const uint8_t UNIT_ID = 1;

// Register storage. Input registers are read-only to the master (BT/ET); the
// single holding register is Artisan's burner-power command.
static const uint16_t IR_COUNT = 2;   // IR0=BT×10, IR1=ET×10
static const uint16_t HR_COUNT = 1;   // HR0=burner power 0..100
static volatile uint16_t inputRegs[IR_COUNT]   = {0, 0};
static volatile uint16_t holdingRegs[HR_COUNT] = {0};

// millis() of the last MODBUS request from Artisan (0 = none yet). Stamped by
// every worker; the control loop reads it to know whether Artisan is live.
static volatile uint32_t lastReqMs = 0;
static inline void touchLink() { lastReqMs = millis(); }

// °C -> unsigned register (×10), clamped. Roaster temps are >= 0; a NaN (sensor
// fault) or negative reading publishes 0 rather than wrapping to a huge value.
static uint16_t toReg10(float c) {
  if (isnan(c) || c <= 0.0f) return 0;
  long v = lroundf(c * 10.0f);
  if (v > 65535) v = 65535;
  return (uint16_t)v;
}

// FC04 — read input registers (BT/ET telemetry).
static ModbusMessage fcReadInput(ModbusMessage request) {
  touchLink();
  uint16_t addr = 0, words = 0;
  request.get(2, addr);
  request.get(4, words);
  if (words == 0 || (uint32_t)addr + words > IR_COUNT)
    return ModbusMessage(request.getServerID(), request.getFunctionCode(), ILLEGAL_DATA_ADDRESS);
  ModbusMessage response;
  response.add(request.getServerID(), request.getFunctionCode(), (uint8_t)(words * 2));
  for (uint16_t i = 0; i < words; i++) response.add((uint16_t)inputRegs[addr + i]);
  return response;
}

// FC03 — read holding registers (read back the burner-power command).
static ModbusMessage fcReadHolding(ModbusMessage request) {
  touchLink();
  uint16_t addr = 0, words = 0;
  request.get(2, addr);
  request.get(4, words);
  if (words == 0 || (uint32_t)addr + words > HR_COUNT)
    return ModbusMessage(request.getServerID(), request.getFunctionCode(), ILLEGAL_DATA_ADDRESS);
  ModbusMessage response;
  response.add(request.getServerID(), request.getFunctionCode(), (uint8_t)(words * 2));
  for (uint16_t i = 0; i < words; i++) response.add((uint16_t)holdingRegs[addr + i]);
  return response;
}

// FC06 — write single holding register (Artisan's usual burner-power write).
static ModbusMessage fcWriteHolding(ModbusMessage request) {
  touchLink();
  uint16_t addr = 0, value = 0;
  request.get(2, addr);
  request.get(4, value);
  if (addr >= HR_COUNT)
    return ModbusMessage(request.getServerID(), request.getFunctionCode(), ILLEGAL_DATA_ADDRESS);
  holdingRegs[addr] = value;
  return ModbusMessage(request);   // FC06 echoes the request back on success
}

// FC16 — write multiple holding registers (some masters use this even for one).
static ModbusMessage fcWriteMultiple(ModbusMessage request) {
  touchLink();
  uint16_t addr = 0, words = 0;
  uint8_t  bytes = 0;
  request.get(2, addr);
  request.get(4, words);
  request.get(6, bytes);
  if (words == 0 || (uint32_t)addr + words > HR_COUNT || bytes != words * 2)
    return ModbusMessage(request.getServerID(), request.getFunctionCode(), ILLEGAL_DATA_ADDRESS);
  for (uint16_t i = 0; i < words; i++) {
    uint16_t v = 0;
    request.get(7 + i * 2, v);
    holdingRegs[addr + i] = v;
  }
  ModbusMessage response;
  response.add(request.getServerID(), request.getFunctionCode(), addr, words);
  return response;
}

void modbusBegin() {
  // Register the workers once; they live on the base server and survive stop/start,
  // so modbusSetActive() can open/close the listener freely by mode.
  mbServer.registerWorker(UNIT_ID, READ_INPUT_REGISTER,  &fcReadInput);
  mbServer.registerWorker(UNIT_ID, READ_HOLD_REGISTER,   &fcReadHolding);
  mbServer.registerWorker(UNIT_ID, WRITE_HOLD_REGISTER,  &fcWriteHolding);
  mbServer.registerWorker(UNIT_ID, WRITE_MULT_REGISTERS, &fcWriteMultiple);
}

void modbusSetActive(bool active) {
  if (active == mbServer.isRunning()) return;   // idempotent — only act on change
  if (active) {
    // port, max concurrent clients, idle timeout (ms). The idle timeout MUST exceed
    // Artisan's sampling interval: the server closes any client idle for longer, so
    // a value below the poll period drops the socket between polls and Artisan flaps
    // between "Connected" and "Modbus Communication Error". 60 s covers any sane
    // sampling rate while still reaping a genuinely dead socket. Extra client slots
    // absorb reconnect races (a dropped socket lingers until the timeout fires).
    const uint8_t  MB_MAX_CLIENTS  = 4;
    const uint32_t MB_IDLE_TIMEOUT = 60000;
    mbServer.start(config.artisan.port, MB_MAX_CLIENTS, MB_IDLE_TIMEOUT);
    Serial.print(F("[modbus] Artisan TCP slave up on :"));
    Serial.println(config.artisan.port);
  } else {
    mbServer.stop();   // drops any Artisan connection and closes the port
    Serial.println(F("[modbus] Artisan TCP slave closed (not in Artisan mode)"));
  }
}

void modbusPublishTemps(float btC, float etC) {
  inputRegs[0] = toReg10(btC);
  inputRegs[1] = toReg10(etC);
}

uint16_t modbusBurnerPower() { return holdingRegs[0]; }

bool modbusLinked(uint32_t withinMs) {
  uint32_t last = lastReqMs;
  return last != 0 && (millis() - last) < withinMs;
}
