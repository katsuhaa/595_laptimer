#include "peakhold.h"

namespace lt {

void PeakHold::reset() {
  rpmOk_ = false;
  rpm_ = 0.0f;
  peak_ = -1.0f;
  setMs_ = 0;
  lastMs_ = 0;
  showOk_ = false;
  showFromMs_ = 0;
  show_ = false;
  low_ = -1.0f;
  top_ = -1.0f;
}

// Same steps as peakTrack() of the round gauge. Calling it twice at the same time changes nothing.
void PeakHold::update(bool rpmOk, float rpm, uint32_t nowMs) {
  if (!rpmOk) {
    reset();
    lastMs_ = nowMs;
    return;
  }
  rpmOk_ = true;
  rpm_ = rpm;
  if (peak_ < 0.0f || rpm >= peak_) {
    peak_ = rpm;
    setMs_ = nowMs;
  } else if (static_cast<uint32_t>(nowMs - setMs_) > cfg_.holdMs) {
    const uint32_t from =
        static_cast<uint32_t>(lastMs_ - setMs_) > cfg_.holdMs ? lastMs_ : setMs_ + cfg_.holdMs;
    peak_ -= cfg_.fallRpmPerS * static_cast<float>(static_cast<uint32_t>(nowMs - from)) / 1000.0f;
    if (peak_ < rpm) peak_ = rpm;
  }
  lastMs_ = nowMs;
  if (low_ < 0.0f || rpm < low_) low_ = rpm;
  if (rpm >= cfg_.minRpm && rpm >= low_ + cfg_.riseRpm) {
    showOk_ = true;
    showFromMs_ = nowMs;
    low_ = rpm;
  }
  show_ = showOk_ && static_cast<uint32_t>(nowMs - showFromMs_) < cfg_.showMs;
  if (show_ && peak_ >= cfg_.minRpm) {
    if (peak_ > top_) top_ = peak_;
  } else {
    top_ = -1.0f;
  }
}

}  // namespace lt
