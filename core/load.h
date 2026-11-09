// core/load.h - overload time (過負荷時間) and cool-down: port of the round gauge's `load` (katsuhaa/abarth595-
// round-gauge 2.15.3, meter_display/app.cpp namespace load, config.h LOAD_*; ui_spec.md 7). Same rules:
//  - with the rpm at or above 「過負荷 回転数」 the value grows 1 s per s, below it it shrinks 1 s per s (not below 0,
//    at most LOAD_MAX_MS). Below the boundary for 「過負荷がリセットされる時間」 -> 0. No rpm signal: no change (the
//    reset count starts again from 0)
//  - colour: 0 = white, 1 = yellow (>= 黄), 2 = red (>= 赤); 0 minutes = that colour off; both 0 = feature off
//  - shrinks while the device is off too: the caller saves value / UTC / time below the boundary every minute
//    (restore at start-up) and onTime() subtracts the time it was off, once, when the time is known
//  - cool-down: after the value once reached yellow (red when yellow is off) and the rpm has been below the
//    boundary for LOAD_COOL_SHOW_MS: the title becomes クールダウン with the time left until the reset
#pragma once

#include <cstdint>

#include "veh.h"

namespace lt {

constexpr uint32_t LOAD_MAX_MS = 35999UL * 1000UL;  // 9:59:59
constexpr uint32_t LOAD_COOL_SHOW_MS = 60000;       // cool-down shown after 1 min below the boundary

struct LoadSet {             // the settings it uses (round gauge Settings)
  int loadRpm = 3000;        // 過負荷 回転数
  int loadYellowMin = 10;    // 過負荷時間 黄 (0 = off)
  int loadRedMin = 30;       // 過負荷時間 赤 (0 = off)
  int loadResetMin = 5;      // 過負荷がリセットされる時間
};

class Load {
 public:
  void update(const VehValues& v, const LoadSet& set, uint32_t nowMs);  // every loop (any screen)
  static bool enabled(const LoadSet& set) { return set.loadYellowMin > 0 || set.loadRedMin > 0; }
  uint32_t valueMs() const { return value_; }
  uint8_t level(const LoadSet& set) const;   // 0 white, 1 yellow, 2 red
  // from yellow (red when yellow is off) until back at 0: the circuit screen shows the overload time
  bool warned(const LoadSet& set) const { return value_ > 0 && (hot_ || level(set) >= 1); }
  bool coolDown(uint32_t nowMs) const { return hot_ && value_ > 0 && lowMs(nowMs) >= LOAD_COOL_SHOW_MS; }
  uint32_t coolLeftMs(const LoadSet& set, uint32_t nowMs) const;  // until the reset
  uint32_t lowMs(uint32_t nowMs) const { return lowSince_ ? nowMs - lowSince_ : 0; }  // below the boundary for
  // start-up: the values saved by the previous run (savedUtc 0 = that run never knew the time)
  void restore(uint32_t valueMs, uint32_t savedUtc, uint32_t lowMs);
  void onTime(uint32_t utc, const LoadSet& set, uint32_t nowMs);  // first call after start-up subtracts the off time
  void capMs(uint32_t ms);  // 累積時間 リセット: the overload time is not more than the cumulative time
  void testSet(uint32_t valueMs, bool hot);  // tests: value and "reached yellow since the reset"
  void testReset();

 private:
  void hotTrack(const LoadSet& set);
  uint32_t value_ = 0;
  uint32_t lastMs_ = 0;
  bool started_ = false;
  uint32_t lowSince_ = 0;  // when the rpm went below the boundary (0 = at or above / no signal)
  bool hot_ = false;       // reached yellow (red when yellow is off) since the last reset
  bool haveSaved_ = false;
  uint32_t savedUtc_ = 0, savedLow_ = 0;
  bool timeDone_ = false;
};

}  // namespace lt
