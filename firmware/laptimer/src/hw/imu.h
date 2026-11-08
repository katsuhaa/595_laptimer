// imu.h - QMI8658 6-axis IMU (I2C 0x6B) on the shared bus of touch.h.
//
// Register sequence of the round gauge imuBegin()/imuPoll() (meter_display.ino, ported there from SensorLib 0.3.1):
// soft reset, wait for the reset-done flag, WHO_AM_I, CTRL1/2/3/5/7, burst read of AX_L..GZ_H. Only the range and
// rate fields differ: accel +-8 g, gyro +-512 dps (as the round gauge), ODR code 5 for both = 224.2 Hz with both
// sensors on (code 6, the round gauge's, is 112 Hz). Range/ODR codes and scale factors: Waveshare 03_I2C_QMI8658
// qmi8658c.h enums and qmi8658c.cpp ssvt_a / ssvt_g.
// Values are in the board (sensor) frame: g and deg/s, no orientation applied (lt::ImuCal does that).
#pragma once
#include <stdint.h>

namespace hw {

constexpr uint8_t IMU_WHO_AM_I = 0x05;       // register 0x00 (round gauge imuBegin, demo qmi8658_get_id)
constexpr float IMU_ODR_HZ = 224.2f;         // new samples per second
constexpr float IMU_ACCEL_RANGE_G = 8.0f;
constexpr float IMU_GYRO_RANGE_DPS = 512.0f;

struct ImuInfo {
  bool ok;          // last imuBegin() succeeded
  uint8_t whoAmI;   // value read by the last imuBegin()/imuWhoAmI() (0x05 expected)
  uint8_t ctrl[8];  // CTRL1..CTRL8 read back at the end of imuBegin() (diagnostics only)
  uint32_t reads;   // imuRead() calls
  uint32_t errors;  // imuRead() calls with an I2C error
};

bool imuBegin();  // reset + WHO_AM_I + configuration; false = no answer, wrong id or a write failed
bool imuRead(float* ax, float* ay, float* az, float* gx, float* gy, float* gz);  // latest sample; false = I2C error
bool imuWhoAmI(uint8_t* id);  // reads register 0x00 for the self-test; true = I2C ok (compare *id with IMU_WHO_AM_I)
// 24-bit sample time stamp TIMESTAMP_L/M/H (0x30..0x32, demo qmi8658_read_timestamp) for measuring the real rate in
// the self-test: (count1 - count0) & 0xFFFFFF over a known time. Unverified until the board is here: the datasheet
// counts one per sample, so about IMU_ODR_HZ per second is expected.
bool imuSampleCount(uint32_t* count);
void imuInfo(ImuInfo* out);

}  // namespace hw
