// core/imucal.h - mounting-angle correction of the IMU. Port of the round gauge's imu
// (katsuhaa/abarth595-round-gauge 05ab0a1, meter_display/app.cpp `imu`, config.h IMU_*), same values
// and steps: "up" from gravity while stopped (3 s), "forward" from the launch acceleration
// (3 -> 15 km/h), long-term average of "up" while driving, re-learning after a re-mount, gravity
// scale, export/import for NVS. Added: the gyro zero (bias) and yaw rate the round gauge computes in
// meter_display.ino imuPoll (bias follows the gyro while still, 0.02 per sample).
// Call sample() at IMU_SAMPLE_HZ (50 Hz) like the round gauge: the stillness test (change between
// samples), the sample counts and the bias step assume 50 Hz. Speed: CAN speed when available
// (not simulated), else the GNSS speed of a fix younger than 1 s (round gauge imuPoll).
// Frames: board (sensor) axes in, car axes out: lon forward +, lat right + (IMU_LAT_SIGN), vert up +.
#pragma once

#include <cstdint>

namespace lt {

constexpr int IMU_SAMPLE_HZ = 50;
constexpr int IMU_LAT_SIGN = 1;              // lateral sign (1 = right positive)
constexpr float IMU_STILL_KMH = 0.5f;        // slower = stopped
constexpr uint32_t IMU_LEVEL_STILL_MS = 3000;  // stopped this long: learn "up"
constexpr float IMU_QUIET_G = 0.03f;         // no speed: change from the previous sample below this = still
constexpr float IMU_QUIET_TILT_COS = 0.707f;  // ... and within 45 deg of the default "up"
constexpr float IMU_UP_EMA_MIN_KMH = 5.0f;   // long-term "up" average only at this speed or faster
constexpr uint32_t IMU_UP_TAU_MS = 600000;   // its time constant (10 min)
constexpr float IMU_FWD_FROM_KMH = 3.0f;     // launch window for "forward"
constexpr float IMU_FWD_TO_KMH = 25.0f;
constexpr float IMU_FWD_DONE_KMH = 15.0f;    // average taken here (reversing does not get this fast)
constexpr int IMU_FWD_SAMPLES = 40;          // minimum accelerating samples (0.8 s at 50 Hz)
constexpr int IMU_RELEARN_COUNT = 5;         // this many very different results in a row = re-mounted
constexpr float IMU_GYRO_BIAS_K = 0.02f;     // gyro zero follows the gyro while still (1 s at 50 Hz)

// Defaults before learning, in sensor axes. Round gauge (1.75 board, screen standing, facing the
// driver): +X up, +Z = back of the board = forward. The square board is mounted the same way
// (standing, screen to the driver) but the QMI8658 axes of the ESP32-S3-Touch-AMOLED-2.41 are NOT
// confirmed yet: check at F1 (stand it as mounted: the axis reading +1 g is "up"; lay it face down:
// the axis reading +1 g is the back = forward) and pass the result to the constructor.
inline constexpr float IMU_DEF_UP[3] = {1.0f, 0.0f, 0.0f};
inline constexpr float IMU_DEF_FWD[3] = {0.0f, 0.0f, 1.0f};

class ImuCal {
 public:
  struct V3 {
    float x, y, z;
  };
  ImuCal() : ImuCal(IMU_DEF_UP, IMU_DEF_FWD) {}
  ImuCal(const float defUp[3], const float defFwd[3]);
  void begin();  // forget what was learned (defaults again)
  void reset() { begin(); }
  // One sample [g] in sensor axes with the vehicle speed of that moment
  void sample(float ax, float ay, float az, bool speedOk, float speedKmh, uint32_t nowMs);
  bool levelOk() const { return levelOk_; }    // "up" learned
  bool forwardOk() const { return fwdOk_; }    // "forward" learned
  bool stillNow() const;                       // still for 1 s or more
  float gravity() const { return g0_; }        // gravity measured while stopped [g] (1 before)
  // Last sample in car axes [g], divided by gravity(), gravity removed from vert (round gauge imu::car)
  void car(float* lon, float* lat, float* vert) const;
  // The same for any accelerometer sample (e.g. every 200 Hz sample between the 50 Hz sample() calls)
  void carOf(float ax, float ay, float az, float* lon, float* lat, float* vert) const;
  // Any sensor-axis vector to car axes (no scaling, no gravity removal; e.g. the gyro)
  void toCar(float x, float y, float z, float* lon, float* lat, float* vert) const;
  // 9 floats: up xyz, forward xyz, up learned 1/0, forward learned 1/0, gravity
  void exportCal(float out[9]) const;
  void importCal(const float in[9]);
  uint32_t calVersion() const { return calVer_; }  // changes when the learned result moves (save trigger)
  // Gyro zero: call once per sample() with the gyro [deg/s] of the same moment
  void updateGyroBias(float gx, float gy, float gz);
  bool gyroBiasOk() const { return biasOk_; }
  void gyroBias(float out[3]) const;
  // Yaw rate [deg/s], left turn positive: gyro minus its zero, about the car's "up" axis
  float yawDps(float gx, float gy, float gz) const;

 private:
  V3 unit(V3 a) const;
  V3 horiz(V3 v, V3 up) const;
  void setUp(V3 up, bool forgetFwd);
  void refreshUp(V3 up);
  void setGravity(float m);

  V3 defUp_, defFwd_;
  V3 up_{}, fwd_{};
  bool levelOk_ = false, fwdOk_ = false;
  uint32_t calVer_ = 0;
  V3 upAtVer_{}, fwdAtVer_{};
  float g0_ = 1.0f;
  int upReject_ = 0, fwdReject_ = 0;
  V3 last_{};
  uint32_t lastMs_ = 0;
  V3 lt_{};  // long-term "up" average while driving
  V3 stillSum_{};
  int stillN_ = 0;
  uint32_t stillStart_ = 0;  // start of the current 3 s window
  uint32_t stillSince_ = 0;  // start of the stillness (across windows; stillNow)
  bool still_ = false;
  V3 fwdSum_{};
  int fwdN_ = 0;
  bool fwdDone_ = false;  // this launch was used
  float prevKmh_ = 0.0f;
  uint32_t prevKmhMs_ = 0;
  bool accel_ = false;
  float bias_[3] = {0.0f, 0.0f, 0.0f};
  bool biasOk_ = false;
};

}  // namespace lt
