// core/peakhold.h - rpm peak hold with the same behaviour and values as the round gauge
// (katsuhaa/abarth595-round-gauge 05ab0a1, meter_display/ui.cpp peakTrack(), peakVisible(),
// peakHolding(); config.h TACH_PEAK_HOLD_MS, TACH_PEAK_FALL_RPM_PER_S, TACH_PEAK_SHOW_MS,
// PEAK_RISE_RPM, PEAK_MIN_RPM, PEAK_NUM_DROP_RPM).
//  - the marker follows the highest rpm, stays holdMs after it, then falls at fallRpmPerS
//    towards the current rpm
//  - it is shown from a clear rise (minRpm or more, riseRpm above the lowest rpm since the
//    last clear rise) until showMs later, and disappears when it falls below minRpm
//  - the centre number is the highest marker value while it is shown
#pragma once

#include <cstdint>

namespace lt {

struct PeakHoldConfig {
  uint32_t holdMs = 1500;
  float fallRpmPerS = 2000.0f;
  uint32_t showMs = 5000;
  float riseRpm = 200.0f;
  float minRpm = 3000.0f;
  float numDropRpm = 100.0f;  // "holding" (past the peak) when rpm is this far below the top
};

class PeakHold {
 public:
  explicit PeakHold(const PeakHoldConfig& cfg = PeakHoldConfig()) : cfg_(cfg) {}
  void reset();
  void update(bool rpmOk, float rpm, uint32_t nowMs);  // every rpm sample

  bool visible() const { return rpmOk_ && peak_ >= cfg_.minRpm && show_; }
  float markerRpm() const { return peak_; }  // where the triangles are
  float topRpm() const { return top_; }      // the centre number; < 0 when not shown
  bool holding() const { return top_ > 0.0f && rpmOk_ && rpm_ <= top_ - cfg_.numDropRpm; }

 private:
  PeakHoldConfig cfg_;
  bool rpmOk_ = false;
  float rpm_ = 0.0f;
  float peak_ = -1.0f;
  uint32_t setMs_ = 0;
  uint32_t lastMs_ = 0;
  bool showOk_ = false;
  uint32_t showFromMs_ = 0;
  bool show_ = false;
  float low_ = -1.0f;
  float top_ = -1.0f;
};

}  // namespace lt
