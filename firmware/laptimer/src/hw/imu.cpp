// imu.cpp - see imu.h
#include "imu.h"

#include <Arduino.h>

#include "freertos/FreeRTOS.h"
#include "touch.h"

namespace hw {
namespace {

constexpr uint8_t kAddr = 0x6B;  // round gauge IMU_ADDR; demo I2C_writr_buff(0x6b, ...)

// Registers (demo qmi8658c.h enum Qmi8658Register; round gauge imuBegin/imuPoll)
constexpr uint8_t kWhoAmI = 0x00;
constexpr uint8_t kCtrl1 = 0x02;
constexpr uint8_t kCtrl2 = 0x03;
constexpr uint8_t kCtrl3 = 0x04;
constexpr uint8_t kCtrl5 = 0x06;
constexpr uint8_t kCtrl7 = 0x08;
constexpr uint8_t kTimestampL = 0x30;
constexpr uint8_t kAxL = 0x35;
constexpr uint8_t kResetDone = 0x4D;  // reads 0x80 when the soft reset has finished (round gauge, max ~15 ms)
constexpr uint8_t kReset = 0x60;

// Values
constexpr uint8_t kResetCmd = 0xB0;
constexpr uint8_t kCtrl1Val = 0x40;                // address auto increment, little endian (round gauge)
constexpr uint8_t kCtrl2Val = (0x02 << 4) | 0x05;  // Qmi8658AccRange_8g | ODR code 5
constexpr uint8_t kCtrl3Val = (0x05 << 4) | 0x05;  // Qmi8658GyrRange_512dps | ODR code 5
constexpr uint8_t kCtrl5Val = 0x77;                // LPF on both, mode 3 (13.37 % of ODR) (round gauge)
constexpr uint8_t kCtrl7Val = 0x03;                // accel + gyro enabled (round gauge)

// Scale: demo qmi8658_config_acc ssvt_a = 1 << 12 LSB/g for 8 g; qmi8658_config_gyro ssvt_g = 64 LSB/dps for 512 dps
constexpr float kAccelScale = 1.0f / 4096.0f;
constexpr float kGyroScale = 1.0f / 64.0f;

ImuInfo g_info = {};
portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;

bool wr(uint8_t reg, uint8_t v) {
  return i2cWriteReg(kAddr, reg, &v, 1);
}

int16_t le16(const uint8_t* b) {
  return (int16_t)(uint16_t)(b[0] | (b[1] << 8));
}

}  // namespace

bool imuBegin() {
  uint8_t v = 0;
  uint8_t ctrl[8] = {0};
  if (!i2cLock(2000)) return false;  // the whole sequence at once (round gauge i2cLock around imuBegin)
  bool ok = wr(kReset, kResetCmd);
  if (ok) {
    ok = false;
    for (int i = 0; i < 50 && !ok; i++) {  // round gauge: up to 50 x 10 ms for 0x4D == 0x80
      delay(10);
      ok = i2cReadReg(kAddr, kResetDone, &v, 1) && v == 0x80;
    }
  }
  uint8_t id = 0;
  if (ok) ok = i2cReadReg(kAddr, kWhoAmI, &id, 1) && id == IMU_WHO_AM_I;
  if (ok) {
    ok = wr(kCtrl1, kCtrl1Val) && wr(kCtrl2, kCtrl2Val) && wr(kCtrl3, kCtrl3Val) && wr(kCtrl5, kCtrl5Val) &&
         wr(kCtrl7, kCtrl7Val);
  }
  if (ok) i2cReadReg(kAddr, kCtrl1, ctrl, sizeof ctrl);  // demo qmi8658_dump_reg(): CTRL1..CTRL8, for the report
  i2cUnlock();
  portENTER_CRITICAL(&g_lock);
  g_info.ok = ok;
  g_info.whoAmI = id;
  for (int i = 0; i < 8; i++) g_info.ctrl[i] = ctrl[i];
  portEXIT_CRITICAL(&g_lock);
  return ok;
}

bool imuRead(float* ax, float* ay, float* az, float* gx, float* gy, float* gz) {
  uint8_t b[12];
  const bool ok = i2cReadReg(kAddr, kAxL, b, sizeof b);  // AX_L..GZ_H (round gauge imuPoll)
  portENTER_CRITICAL(&g_lock);
  g_info.reads++;
  if (!ok) g_info.errors++;
  portEXIT_CRITICAL(&g_lock);
  if (!ok) return false;
  if (ax) *ax = (float)le16(b + 0) * kAccelScale;
  if (ay) *ay = (float)le16(b + 2) * kAccelScale;
  if (az) *az = (float)le16(b + 4) * kAccelScale;
  if (gx) *gx = (float)le16(b + 6) * kGyroScale;
  if (gy) *gy = (float)le16(b + 8) * kGyroScale;
  if (gz) *gz = (float)le16(b + 10) * kGyroScale;
  return true;
}

bool imuWhoAmI(uint8_t* id) {
  uint8_t v = 0;
  const bool ok = i2cReadReg(kAddr, kWhoAmI, &v, 1);
  if (id) *id = v;
  if (ok) {
    portENTER_CRITICAL(&g_lock);
    g_info.whoAmI = v;
    portEXIT_CRITICAL(&g_lock);
  }
  return ok;
}

bool imuSampleCount(uint32_t* count) {
  uint8_t b[3];
  if (!i2cReadReg(kAddr, kTimestampL, b, sizeof b)) return false;
  if (count) *count = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16);
  return true;
}

void imuInfo(ImuInfo* out) {
  if (!out) return;
  portENTER_CRITICAL(&g_lock);
  *out = g_info;
  portEXIT_CRITICAL(&g_lock);
}

}  // namespace hw
