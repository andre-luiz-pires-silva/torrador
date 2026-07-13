#include <Arduino.h>
#include <U8g2lib.h>
#include "board_config.h"
#include "branding.h"

// -----------------------------------------------------------------------------
// First-steps sketch (step 1): read ONE MAX6675 thermocouple and show the
// reading on a 0.96" SSD1306 OLED, with the white-label brand name on top.
//
// No Wi-Fi, no web UI, no relays yet — this validates the SPI sensor read, the
// I2C display and the branding path end to end. Wiring (see chat):
//   MAX6675: VCC->3V3, GND->GND, SCK->GPIO18, CS->GPIO5, SO->GPIO19
//   OLED   : VCC->3V3, GND->GND, SDA->GPIO21, SCL->GPIO22
// -----------------------------------------------------------------------------

// SSD1306 128x64 over hardware I2C (ESP32 default pins 21/SDA, 22/SCL). Full
// frame buffer (_F_): compose the whole screen, then push it in one transfer.
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);

// MAX6675 read cadence. The chip needs ~220ms between conversions; reading
// faster returns stale data (CLAUDE.md safety rule).
static const uint32_t SENSOR_INTERVAL_MS = 250;
static uint32_t lastReadMs = 0;
static float lastTempC = NAN;

// Bit-bang one 16-bit frame from a MAX6675 on the shared SCK/SO bus.
// Returns temperature in °C, or NAN if the thermocouple is open/disconnected.
static float max6675ReadCelsius(uint8_t csPin) {
  digitalWrite(csPin, LOW);
  delayMicroseconds(2);  // let SO settle after CS falls

  uint16_t value = 0;
  for (int8_t bit = 15; bit >= 0; bit--) {
    digitalWrite(PIN_MAX6675_SCK, HIGH);
    delayMicroseconds(2);
    if (digitalRead(PIN_MAX6675_SO)) {
      value |= (1u << bit);
    }
    digitalWrite(PIN_MAX6675_SCK, LOW);
    delayMicroseconds(2);
  }

  digitalWrite(csPin, HIGH);

  if (value & 0x0004) {   // bit D2 set -> no thermocouple attached
    return NAN;
  }
  value >>= 3;            // drop the 3 status bits; 12-bit reading remains
  return value * 0.25f;   // 0.25 °C per LSB
}

// Draw a UTF-8 string horizontally centred at baseline y, using the current font.
static void drawCentered(const char *text, int16_t y) {
  int16_t w = display.getUTF8Width(text);
  display.drawUTF8((128 - w) / 2, y, text);
}

static void renderScreen() {
  display.clearBuffer();

  // --- Brand header (white-label) ---
  display.setFont(u8g2_font_helvB08_tf);  // _tf = full glyph set (accents, &)
  drawCentered(BRAND_NAME, 10);
  display.drawHLine(0, 13, 128);

  // --- Reading label ---
  display.setFont(u8g2_font_6x10_tf);
  drawCentered("BT", 28);

  char num[8];
  if (isnan(lastTempC)) {
    snprintf(num, sizeof(num), "--");
  } else {
    snprintf(num, sizeof(num), "%.1f", lastTempC);
  }

  // Big number + smaller unit, drawn as one centred group.
  display.setFont(u8g2_font_fub20_tr);
  int16_t wNum = display.getUTF8Width(num);
  display.setFont(u8g2_font_helvB08_tf);
  int16_t wUnit = display.getUTF8Width("°C");
  const int16_t gap = 3;
  int16_t x0 = (128 - (wNum + gap + wUnit)) / 2;

  display.setFont(u8g2_font_fub20_tr);
  display.drawUTF8(x0, 58, num);
  display.setFont(u8g2_font_helvB08_tf);
  display.drawUTF8(x0 + wNum + gap, 50, "°C");

  display.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(100);

  // MAX6675 bus: SCK idle low, CS idle high, SO input.
  pinMode(PIN_MAX6675_SCK, OUTPUT);
  digitalWrite(PIN_MAX6675_SCK, LOW);
  pinMode(PIN_MAX6675_CS_BT, OUTPUT);
  digitalWrite(PIN_MAX6675_CS_BT, HIGH);
  pinMode(PIN_MAX6675_SO, INPUT);

  display.begin();

  // Splash while the MAX6675 completes its first conversion.
  display.clearBuffer();
  display.setFont(u8g2_font_helvB08_tf);
  drawCentered(BRAND_NAME, 28);
  display.setFont(u8g2_font_6x10_tf);
  drawCentered("Iniciando...", 44);
  display.sendBuffer();

  Serial.println();
  Serial.println(F("[torrador] boot ok — step 1: MAX6675 -> OLED"));
  Serial.print(F("[torrador] brand: "));
  Serial.println(BRAND_NAME);

  // First conversion takes up to ~220ms after power-up.
  delay(300);
  lastTempC = max6675ReadCelsius(PIN_MAX6675_CS_BT);
  lastReadMs = millis();
  renderScreen();
}

void loop() {
  uint32_t now = millis();
  if (now - lastReadMs >= SENSOR_INTERVAL_MS) {
    lastReadMs = now;
    lastTempC = max6675ReadCelsius(PIN_MAX6675_CS_BT);

    if (isnan(lastTempC)) {
      Serial.println(F("[torrador] BT: thermocouple open"));
    } else {
      Serial.print(F("[torrador] BT: "));
      Serial.print(lastTempC, 2);
      Serial.println(F(" C"));
    }
    renderScreen();
  }
}
