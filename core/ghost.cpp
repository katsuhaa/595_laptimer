#include "ghost.h"

#include <cmath>

namespace lt {

void Ghost::begin(TracePoint* bufA, TracePoint* bufB, int capacity) {
  best_ = bufA;
  cur_ = bufB;
  cap_ = capacity;
  bestN_ = 0;
  curN_ = 0;
  overflow_ = false;
  inLap_ = false;
  lost_ = true;
}

bool Ghost::setBest(const TracePoint* pts, int n) {
  if (n < 2 || n > cap_) return false;
  for (int i = 0; i < n; ++i) best_[i] = pts[i];
  bestN_ = n;
  return true;
}

void Ghost::push(int64_t tUs, Vec2 pos) {
  if (curN_ >= cap_) {
    overflow_ = true;
    return;
  }
  cur_[curN_++] = {pos.x, pos.y, static_cast<float>(tUs - lapStartUs_) * 1e-6f};
}

void Ghost::startLap(int64_t tUs, Vec2 pos) {
  lapStartUs_ = tUs;
  curN_ = 0;
  overflow_ = false;
  inLap_ = true;
  match_ = 0;
  lost_ = false;
  push(tUs, pos);
}

void Ghost::addSample(int64_t tUs, Vec2 pos) {
  if (inLap_ && tUs > lapStartUs_) push(tUs, pos);
}

void Ghost::endLap(int64_t tUs, Vec2 pos, bool keepAsBest) {
  if (!inLap_) return;
  push(tUs, pos);
  inLap_ = false;
  if (keepAsBest && !overflow_ && curN_ >= 2) {
    TracePoint* t = best_;  // swap buffers instead of copying
    best_ = cur_;
    cur_ = t;
    bestN_ = curN_;
  }
  curN_ = 0;
}

bool Ghost::nearest(Vec2 p, int from, int to, float nowS, bool timeWindow, int* seg, float* u,
                    float* d2) const {
  bool found = false;
  float bestD2 = 0.0f;
  for (int i = from; i <= to; ++i) {
    const TracePoint& a = best_[i];
    const TracePoint& b = best_[i + 1];
    if (timeWindow && std::fabs(a.tS - nowS) > maxShiftS) continue;
    const Vec2 ab = {b.x - a.x, b.y - a.y};
    const Vec2 ap = {p.x - a.x, p.y - a.y};
    const float len2 = dot(ab, ab);
    float t = len2 > 0.0f ? dot(ap, ab) / len2 : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const Vec2 d = ap - ab * t;
    const float dd = dot(d, d);
    if (!found || dd < bestD2) {
      found = true;
      bestD2 = dd;
      *seg = i;
      *u = t;
    }
  }
  *d2 = bestD2;
  return found;
}

bool Ghost::delta(int64_t tUs, Vec2 pos, float* deltaS) {
  if (!inLap_ || !hasBest()) return false;
  const float nowS = static_cast<float>(tUs - lapStartUs_) * 1e-6f;
  const float maxD2 = maxOffsetM * maxOffsetM;
  const int lastSeg = bestN_ - 2;
  int seg = 0;
  float u = 0.0f;
  float d2 = 0.0f;
  bool ok = false;
  if (!lost_) {
    const int from = match_ > 2 ? match_ - 2 : 0;
    const int to = match_ + searchAhead < lastSeg ? match_ + searchAhead : lastSeg;
    ok = nearest(pos, from, to, nowS, false, &seg, &u, &d2) && d2 <= maxD2;
  }
  if (!ok) ok = nearest(pos, 0, lastSeg, nowS, true, &seg, &u, &d2) && d2 <= maxD2;
  if (!ok) {
    lost_ = true;
    return false;
  }
  lost_ = false;
  match_ = seg;
  const TracePoint& a = best_[seg];
  const TracePoint& b = best_[seg + 1];
  *deltaS = nowS - (a.tS + (b.tS - a.tS) * u);
  return true;
}

}  // namespace lt
