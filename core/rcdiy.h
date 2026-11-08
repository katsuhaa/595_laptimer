// core/rcdiy.h - contents of the RaceChrono DIY BLE characteristics (github.com/aollin/racechrono-ble-diy-device).
// Port of the round gauge's rcdiy (katsuhaa/abarth595-round-gauge 05ab0a1, meter_display/app.cpp
// `rcdiy`, config.h RC_CAN_MIN_MS, RC_PID_IMU_*), plus the IMU payloads and the GPS sync/time schedule
// that the round gauge does in meter_display.ino (imuPoll, gpsOnPvt).
// Characteristics: 0x0001 CAN (4-byte id LE + 1..16 bytes), 0x0002 PID filter (written by RaceChrono),
// 0x0003 GPS main (20 bytes), 0x0004 GPS time (3 bytes); the 3-bit sync ties 0x0003 to 0x0004.
#pragma once

#include <cstddef>
#include <cstdint>

#include "ubx.h"

namespace lt {
namespace rcdiy {

// Shortest interval per car CAN PID towards RaceChrono, even when RaceChrono asks for less
// (c6_can_link 2.x sends changed ids every 20 ms; keeps the BLE load of 1.x).
constexpr uint16_t RC_CAN_MIN_MS = 50;
// IMU values are sent as CAN PIDs (the car does not use these ids; RaceChrono iOS rejected
// 0x1FFFFF01 / 0x0FFFFF01). Payload int16 LE x 3:
//   RC_PID_IMU_ACCEL: longitudinal, lateral, vertical acceleration [mg] (forward, right, up positive;
//                     gravity removed). RaceChrono formula: bytesToIntLe(raw, 0, 2) * 0.00980665 (m/s2)
//   RC_PID_IMU_GYRO : yaw rate [0.01 deg/s] (left turn positive), the other two are 0
constexpr uint32_t RC_PID_IMU_ACCEL = 0x0618A0F1;
constexpr uint32_t RC_PID_IMU_GYRO = 0x0618A0F2;
constexpr uint32_t PID_IMU_ACCEL = RC_PID_IMU_ACCEL;
constexpr uint32_t PID_IMU_GYRO = RC_PID_IMU_GYRO;
constexpr int MAX_PIDS = 48;  // PID filter table size

// GPS main (20 bytes) and GPS time (3 bytes); sync 0..7; speed from the GNSS
void encodeGpsMain(const GnssFix& f, uint8_t sync, uint8_t out[20]);
void encodeGpsTime(const GnssFix& f, uint8_t sync, uint8_t out[3]);
uint32_t dateHourCode(const GnssFix& f);  // the 21-bit date/hour value of 0x0004
// CAN: 4-byte id (LE) + payload (dlc up to 16). Returns the size.
size_t encodeCan(uint32_t id, const uint8_t* d, uint8_t dlc, uint8_t out[20]);
int16_t clampI16(float v);  // rounded, clamped to int16
// IMU as CAN (id RC_PID_IMU_*). Return the size (10).
size_t encodeImuAccel(float lonG, float latG, float vertG, uint8_t out[20]);
size_t encodeImuGyro(float yawDps, uint8_t out[20]);

// PID filter (commands written to 0x0002). RaceChrono sends "deny all", then "allow PID every n ms"
// per PID. Not thread-safe (the BLE write callback and the senders need a lock).
class Filter {
 public:
  Filter() { reset(); }
  void reset();  // deny all
  void command(const uint8_t* data, size_t len);
  // May this PID be sent now (allowed and its interval passed)? Updates its last time when true.
  // minMs: send no faster than this even when RaceChrono asks for less (car CAN: RC_CAN_MIN_MS; IMU 0)
  bool pass(uint32_t pid, uint32_t nowMs, uint16_t minMs = 0);
  int count() const { return allowAll_ ? -1 : n_; }  // allowed PIDs (-1 = all)
  bool allowed(uint32_t pid) const;                  // asked for (does not touch the interval)

 private:
  bool allowAll_ = false;
  uint16_t allInterval_ = 0;
  int n_ = 0;
  uint32_t pid_[MAX_PIDS]{};
  uint16_t interval_[MAX_PIDS]{};
  uint32_t lastMs_[MAX_PIDS]{};
  // allow-all: last send time per PID (small table; when full, sent without interval)
  uint32_t allPid_[MAX_PIDS]{}, allMs_[MAX_PIDS]{};
  int allN_ = 0;
};

// The round gauge's GPS schedule (gpsOnPvt): the sync advances when the date/hour code changes; the
// time characteristic goes out when it changed and at least once per second (for a phone that just
// connected).
class GpsTimeSync {
 public:
  void reset() { sync_ = 0; prevCode_ = 0xFFFFFFFFu; timeMs_ = 0; }
  // Every new fix (nowMs = its rxMs). Returns true when 0x0004 should be sent with this fix.
  bool update(const GnssFix& f, uint32_t nowMs);
  uint8_t sync() const { return sync_; }

 private:
  uint8_t sync_ = 0;
  uint32_t prevCode_ = 0xFFFFFFFFu;
  uint32_t timeMs_ = 0;
};

}  // namespace rcdiy
}  // namespace lt
