// touch.cpp - see touch.h
#include "touch.h"

#include <Arduino.h>
#include <Wire.h>

#include "../board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace hw {
namespace {

constexpr uint32_t kI2cHz = 300 * 1000;  // Waveshare 2.41 demos (09_LVGL_Test i2c_conf, i2c_bsp I2C_master_Init)

SemaphoreHandle_t g_i2cMu = nullptr;
bool g_i2cOk = false;

// FT5x06 registers (esp_lcd_touch_ft5x06.c)
constexpr uint8_t kTpAddr = 0x38;     // ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS
constexpr uint8_t kTouchPoints = 0x02;  // FT5x06_TOUCH_POINTS
constexpr uint8_t kTouch1Xh = 0x03;     // FT5x06_TOUCH1_XH (XH XL YH YL ... of the first point)
constexpr int kProbeTries = 25;         // touchBegin: answer within 25 x 20 ms
constexpr uint32_t kProbeGapMs = 20;

// Demo transform: esp_lcd_touch_get_coordinates() with x_max = EXAMPLE_LCD_V_RES - 1 = 449,
// y_max = EXAMPLE_LCD_H_RES - 1 = 599, mirror_y then swap_xy (the FT5x06 driver has no hardware mirror/swap)
constexpr int kTpYMax = board::LCD_W - 1;  // 599

TouchStats g_tp = {};
portMUX_TYPE g_tpLock = portMUX_INITIALIZER_UNLOCKED;

int clampInt(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

// ---------------------------------------------------------------- I2C bus

bool i2cBegin() {
  if (g_i2cOk) return true;
  if (!g_i2cMu) g_i2cMu = xSemaphoreCreateRecursiveMutex();
  if (!g_i2cMu) return false;
  // Wire enables the internal pull-ups like the demo (sda/scl_pullup_en = GPIO_PULLUP_ENABLE)
  g_i2cOk = Wire.begin(board::I2C_SDA, board::I2C_SCL, kI2cHz);
  return g_i2cOk;
}

bool i2cLock(uint32_t timeoutMs) {
  if (!g_i2cMu) return false;
  return xSemaphoreTakeRecursive(g_i2cMu, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void i2cUnlock() {
  if (g_i2cMu) xSemaphoreGiveRecursive(g_i2cMu);
}

bool i2cWriteReg(uint8_t addr, uint8_t reg, const uint8_t* data, size_t n) {
  if (!g_i2cOk || !i2cLock()) return false;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (n) Wire.write(data, n);
  const bool ok = Wire.endTransmission() == 0;
  i2cUnlock();
  return ok;
}

// Register read with a repeated start, as the round gauge imuRead()/i2cRead() and the demo
// I2C_read_buff() (i2c_master_write_read_device) do it
bool i2cReadReg(uint8_t addr, uint8_t reg, uint8_t* buf, size_t n) {
  if (!g_i2cOk || !buf || !n || !i2cLock()) return false;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  bool ok = Wire.endTransmission(false) == 0;
  if (ok) ok = Wire.requestFrom(addr, n) == n;
  if (ok) {
    for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)Wire.read();
  }
  i2cUnlock();
  return ok;
}

bool i2cProbe(uint8_t addr) {
  if (!g_i2cOk || !i2cLock()) return false;
  Wire.beginTransmission(addr);
  const bool ok = Wire.endTransmission() == 0;
  i2cUnlock();
  return ok;
}

// ---------------------------------------------------------------- touch

bool touchBegin() {
  if (!i2cBegin()) return false;
  // Reset as esp_lcd_touch_ft5x06.c touch_ft5x06_reset() with the ESP-IDF 09_LVGL_Test config
  // (EXAMPLE_PIN_NUM_TOUCH_RST = GPIO_NUM_3, levels.reset = 0): low 10 ms, high, 10 ms
  pinMode(board::TP_RST, OUTPUT);
  digitalWrite(board::TP_RST, LOW);
  delay(10);
  digitalWrite(board::TP_RST, HIGH);
  delay(10);
  // Answer check with the normal read (round gauge touchBegin: only procedures the official driver uses). The demo
  // never reads right after the reset, so the read is repeated until the controller answers.
  const uint32_t t0 = millis();
  bool ok = false;
  for (int i = 0; i < kProbeTries && !ok; i++) {
    if (i) delay(kProbeGapMs);
    uint8_t points = 0;
    ok = i2cReadReg(kTpAddr, kTouchPoints, &points, 1);
  }
  portENTER_CRITICAL(&g_tpLock);
  g_tp.ok = ok;
  g_tp.readyMs = ok ? millis() - t0 : 0;
  portEXIT_CRITICAL(&g_tpLock);
  return ok;
}

// esp_lcd_touch_ft5x06_read_data(): TOUCH_POINTS, 0 or > 5 = not touched; else the point registers from TOUCH1_XH
// (the demo uses the first point only)
bool touchRead(int* x, int* y) {
  uint8_t points = 0;
  uint8_t d[6] = {0};
  bool pressed = false;
  if (!i2cLock()) return false;
  bool ok = i2cReadReg(kTpAddr, kTouchPoints, &points, 1);
  if (ok && points != 0 && points <= 5) {
    ok = i2cReadReg(kTpAddr, kTouch1Xh, d, sizeof d);
    if (ok) {
      const int rx = ((d[0] & 0x0F) << 8) | d[1];  // panel portrait x 0..449
      const int ry = ((d[2] & 0x0F) << 8) | d[3];  // panel portrait y 0..599
      // mirror_y: y = y_max - y; swap_xy: (x, y) = (y, x)
      if (x) *x = clampInt(kTpYMax - ry, 0, board::LCD_W - 1);
      if (y) *y = clampInt(rx, 0, board::LCD_H - 1);
      pressed = true;
    }
  }
  i2cUnlock();
  portENTER_CRITICAL(&g_tpLock);
  g_tp.reads++;
  if (!ok) g_tp.errors++;
  else g_tp.ok = true;
  portEXIT_CRITICAL(&g_tpLock);
  return pressed;
}

void touchStats(TouchStats* out) {
  if (!out) return;
  portENTER_CRITICAL(&g_tpLock);
  *out = g_tp;
  portEXIT_CRITICAL(&g_tpLock);
}

}  // namespace hw
