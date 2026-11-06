#include "lap.h"

#include <cmath>

namespace lt {

namespace {

Vec2 lerp(Vec2 a, Vec2 b, float s) { return a + (b - a) * s; }

int64_t lerpTime(int64_t t0, int64_t t1, float s) {
  return t0 + static_cast<int64_t>(s * static_cast<float>(t1 - t0) + 0.5f);
}

}  // namespace

LapTimer::Line LapTimer::makeLine(const Plane& pl, const GateDef& d) {
  Line l;
  l.c = pl.toLocal(d.latE7, d.lonE7);
  l.h = headingUnit(d.headingDeg);
  l.n = {l.h.y, -l.h.x};
  l.halfW = 0.5f * d.widthM;
  return l;
}

// Forward crossing of p->q through the line. Half-open (before < 0 <= after), so a fix
// lying exactly on the line is counted once. *frac: position of the crossing on p->q.
bool LapTimer::crosses(const Line& l, Vec2 p, Vec2 q, float* frac) {
  const float dp = dot(p - l.c, l.h);
  const float dq = dot(q - l.c, l.h);
  if (!(dp < 0.0f && dq >= 0.0f)) return false;
  const float s = dp / (dp - dq);
  if (std::fabs(dot(lerp(p, q, s) - l.c, l.n)) > l.halfW) return false;
  *frac = s;
  return true;
}

void LapTimer::begin(const Plane& plane, const CourseDef& course, const LapConfig& cfg) {
  plane_ = plane;
  cfg_ = cfg;
  sf_ = makeLine(plane, course.startFinish);
  numSec_ = course.numSectors;
  if (numSec_ < 0) numSec_ = 0;
  if (numSec_ > kMaxSectors) numSec_ = kMaxSectors;
  for (int i = 0; i < numSec_; ++i) sec_[i] = makeLine(plane, course.sectors[i]);
  minLapUs_ = static_cast<int64_t>(course.minLapS * 1e6f);
  reset();
}

void LapTimer::reset() {
  havePrev_ = false;
  lapStartUs_ = -1;
  completed_ = 0;
  nextSector_ = 0;
  last_ = LapResult{};
  bestUs_ = -1;
  bestNo_ = 0;
  sfCrossUs_ = -1;
}

uint32_t LapTimer::feed(const Fix& f) {
  if (!f.valid) return 0;  // like a missing epoch: the gap check below decides
  if (havePrev_ && f.tUs <= prev_.tUs) return 0;  // duplicate or out of order
  const Vec2 pos = plane_.toLocal(f.latE7, f.lonE7);
  uint32_t ev = 0;
  if (havePrev_) {
    const float dt = static_cast<float>(f.tUs - prev_.tUs) * 1e-6f;
    const bool usable = dt <= cfg_.maxGapS &&
                        prev_.speedMps >= cfg_.minSpeedMps &&
                        f.speedMps >= cfg_.minSpeedMps &&
                        length(pos - prevPos_) <= cfg_.maxJumpMps * dt;
    if (usable) ev = segment(prevPos_, pos, prev_.tUs, f.tUs);
  }
  prev_ = f;
  prevPos_ = pos;
  havePrev_ = true;
  return ev;
}

// One segment between two fixes. When it crosses S/F, the part before the line belongs
// to the lap that ends and the part after it to the lap that starts.
uint32_t LapTimer::segment(Vec2 p, Vec2 q, int64_t t0, int64_t t1) {
  float s = 0.0f;
  if (!crosses(sf_, p, q, &s)) return sector(p, q, t0, t1);
  const Vec2 x = lerp(p, q, s);
  const int64_t tx = lerpTime(t0, t1, s);
  uint32_t ev = sector(p, x, t0, tx);
  ev |= startFinish(x, tx);
  ev |= sector(x, q, tx, t1);
  return ev;
}

// Only the next sector line of the current lap is checked, so they count in order.
uint32_t LapTimer::sector(Vec2 p, Vec2 q, int64_t t0, int64_t t1) {
  if (!timing() || nextSector_ >= numSec_) return 0;
  float s = 0.0f;
  if (!crosses(sec_[nextSector_], p, q, &s)) return 0;
  secCrossUs_[nextSector_++] = lerpTime(t0, t1, s);
  return kEvSector;
}

uint32_t LapTimer::startFinish(Vec2 x, int64_t tx) {
  if (timing() && tx - lapStartUs_ < minLapUs_) return 0;  // too soon: not a lap
  sfCrossUs_ = tx;
  sfCrossPos_ = x;
  if (!timing()) {
    startLap(tx);
    return kEvLapStart;
  }
  LapResult& r = last_;
  r.lapNo = completed_ + 1;
  r.startUs = lapStartUs_;
  r.endUs = tx;
  r.lapUs = tx - lapStartUs_;
  r.numSplits = 0;
  if (nextSector_ == numSec_) {
    int64_t from = lapStartUs_;
    for (int i = 0; i < numSec_; ++i) {
      r.splitUs[i] = secCrossUs_[i] - from;
      from = secCrossUs_[i];
    }
    r.splitUs[numSec_] = tx - from;
    r.numSplits = numSec_ + 1;
  }
  ++completed_;
  uint32_t ev = kEvLap;
  if (bestUs_ < 0 || r.lapUs < bestUs_) {
    bestUs_ = r.lapUs;
    bestNo_ = r.lapNo;
    ev |= kEvBest;
  }
  startLap(tx);
  return ev;
}

void LapTimer::startLap(int64_t tUs) {
  lapStartUs_ = tUs;
  nextSector_ = 0;
}

}  // namespace lt
