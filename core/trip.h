// core/trip.h - cumulative times ▲▼ (累積時間), moving time and full-throttle time: port of the round gauge's
// `trip` (katsuhaa/abarth595-round-gauge 2.15.3, meter_display/app.cpp namespace trip, config.h). Same rules:
//  - ▲ = time with the rpm at or above the boundary (設定の「過負荷 回転数」, 2.12.0), ▼ = below it. Counted only
//    while the rpm signal is there, standing still or moving. Kept over power-off (the caller saves them every
//    minute and gives them back with setTimes at start-up)
//  - moving time = vehicle speed >= TRIP_MOVING_KMH; full-throttle time = moving and throttle >= TRIP_WOT_PCT
//  - a gap between two calls counts at most 500 ms (screen changes etc. do not count too much)
// ▲ is also the over-rpm time of the odometer log (走行記録, core/odolog) and the basis of the maintenance
// (メンテナンスまでの累積時間).
#pragma once

#include <cstdint>

#include "veh.h"

namespace lt {

constexpr float TRIP_MOVING_KMH = 1.0f;  // round gauge MOVING_SPEED_KMH
constexpr float TRIP_WOT_PCT = 95.0f;    // round gauge WOT_THROTTLE_PCT
constexpr int TRIP_RPM_DEFAULT = 3000;   // round gauge TIME_RPM (the boundary is the 過負荷 回転数 setting)
constexpr int TRIP_IN_MAX_H = 99;        // 累積時間 リセット: the value entered goes up to 99:00:00 (round gauge)

class Trip {
 public:
  // call every loop (any screen). boundaryRpm = 設定の過負荷 回転数
  void update(const VehValues& v, uint32_t nowMs, int boundaryRpm = TRIP_RPM_DEFAULT);
  uint32_t movingMs() const { return moving_; }
  uint32_t wotMs() const { return wot_; }
  uint32_t aboveMs() const { return above_; }  // ▲
  uint32_t belowMs() const { return below_; }  // ▼
  void setTimes(uint32_t above, uint32_t below) {  // at start-up from NVS, and 累積時間 リセット (▲ = value, ▼ = 0)
    above_ = above;
    below_ = below;
  }
  void resetTimes() { above_ = below_ = 0; }

 private:
  uint32_t lastMs_ = 0;
  bool started_ = false;
  uint32_t moving_ = 0, wot_ = 0;
  uint32_t above_ = 0, below_ = 0;
};

// メンテナンス (round gauge ui.cpp, ui_spec.md 7): ▲ reached 「メンテナンスまでの累積時間」 (hours, 0 = off)
inline bool tripMaintDue(uint32_t aboveMs, int maintHours) {
  return maintHours > 0 && aboveMs >= static_cast<uint32_t>(maintHours) * 3600000u;
}
// time left until the maintenance [ms] (0 when due or off)
inline uint32_t tripMaintLeftMs(uint32_t aboveMs, int maintHours) {
  if (maintHours <= 0) return 0;
  const uint32_t lim = static_cast<uint32_t>(maintHours) * 3600000u;
  return aboveMs >= lim ? 0 : lim - aboveMs;
}

}  // namespace lt
