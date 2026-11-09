#include "load.h"

namespace lt {

namespace {
uint32_t minMs(int m) { return m > 0 ? static_cast<uint32_t>(m) * 60000UL : 0; }
}  // namespace

void Load::hotTrack(const LoadSet& set) {  // remember yellow, forget at 0
  if (value_ == 0) hot_ = false;
  else if (level(set) >= 1) hot_ = true;
}

void Load::update(const VehValues& v, const LoadSet& set, uint32_t nowMs) {
  uint32_t dt = started_ ? nowMs - lastMs_ : 0;
  started_ = true;
  lastMs_ = nowMs;
  if (dt > 500) dt = 500;  // as Trip
  // time below the boundary (the reset count; no signal starts it again from 0)
  if (v.rpmOk && v.rpm < set.loadRpm) {
    if (!lowSince_) lowSince_ = nowMs ? nowMs : 1;
  } else {
    lowSince_ = 0;
  }
  if (!enabled(set)) {  // off: not counted, 0
    value_ = 0;
    hot_ = false;
    return;
  }
  if (!v.rpmOk) return;  // no signal: no change
  if (v.rpm >= set.loadRpm) value_ = value_ + dt > LOAD_MAX_MS ? LOAD_MAX_MS : value_ + dt;
  else value_ = value_ > dt ? value_ - dt : 0;
  if (lowSince_ && nowMs - lowSince_ >= minMs(set.loadResetMin)) value_ = 0;  // reset
  hotTrack(set);
}

uint8_t Load::level(const LoadSet& set) const {  // no colour for 0; yellow >= red: red first
  if (set.loadRedMin > 0 && value_ >= minMs(set.loadRedMin)) return 2;
  if (set.loadYellowMin > 0 && value_ >= minMs(set.loadYellowMin)) return 1;
  return 0;
}

uint32_t Load::coolLeftMs(const LoadSet& set, uint32_t nowMs) const {
  const uint32_t r = minMs(set.loadResetMin), l = lowMs(nowMs);
  return l >= r ? 0 : r - l;
}

void Load::restore(uint32_t valueMs, uint32_t savedUtc, uint32_t lowMs) {
  value_ = valueMs > LOAD_MAX_MS ? LOAD_MAX_MS : valueMs;
  savedUtc_ = savedUtc;
  savedLow_ = lowMs;
  haveSaved_ = true;
  timeDone_ = false;
}

void Load::onTime(uint32_t utc, const LoadSet& set, uint32_t nowMs) {
  if (timeDone_) return;
  timeDone_ = true;
  if (!haveSaved_ || savedUtc_ == 0 || !enabled(set)) return;  // the previous run never knew the time
  // start-up time (now - time since start-up) - last save of the previous run = time it was off (up to 1 min more)
  const int64_t boot = static_cast<int64_t>(utc) - static_cast<int64_t>(nowMs / 1000);
  const int64_t offS = boot - static_cast<int64_t>(savedUtc_);
  if (offS <= 0) return;
  const uint64_t offMs = static_cast<uint64_t>(offS) * 1000ULL;
  if (static_cast<uint64_t>(savedLow_) + offMs >= static_cast<uint64_t>(minMs(set.loadResetMin))) value_ = 0;
  else value_ = static_cast<uint64_t>(value_) > offMs ? value_ - static_cast<uint32_t>(offMs) : 0;
  hotTrack(set);
}

void Load::capMs(uint32_t ms) {
  if (value_ > ms) value_ = ms;
  if (value_ == 0) hot_ = false;
}

void Load::testSet(uint32_t valueMs, bool hot) {
  value_ = valueMs > LOAD_MAX_MS ? LOAD_MAX_MS : valueMs;
  hot_ = hot && value_ > 0;
  lowSince_ = 0;
  started_ = false;
}

void Load::testReset() {
  value_ = 0;
  hot_ = false;
  lowSince_ = 0;
  started_ = false;
  haveSaved_ = false;
  savedUtc_ = savedLow_ = 0;
  timeDone_ = false;
}

}  // namespace lt
