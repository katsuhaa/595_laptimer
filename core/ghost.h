// core/ghost.h - difference to the best lap at the same place on the track.
// The best lap is kept as a trace of points (position, seconds since its start).
// Each fix is matched to the nearest point on that trace, searching forward from the
// previous match, and the best lap's elapsed time there is compared with the current one.
#pragma once

#include <cstdint>

#include "geo.h"

namespace lt {

struct TracePoint {
  float x;   // local plane [m]
  float y;
  float tS;  // seconds since the lap started
};

class Ghost {
 public:
  // Two buffers of `capacity` points from the caller (PSRAM on the device).
  void begin(TracePoint* bufA, TracePoint* bufB, int capacity);
  void clearBest() { bestN_ = 0; }
  bool hasBest() const { return bestN_ >= 2; }
  float bestLapS() const { return hasBest() ? best_[bestN_ - 1].tS : 0.0f; }

  // At every S/F crossing (interpolated time and position): endLap() then startLap().
  void startLap(int64_t tUs, Vec2 pos);
  void endLap(int64_t tUs, Vec2 pos, bool keepAsBest);
  // Every fix while a lap is running.
  void addSample(int64_t tUs, Vec2 pos);

  // Elapsed time now minus the best lap's elapsed time at the nearest point of its trace.
  // Negative = ahead of the best lap. false: no best lap, no lap running, or off its line.
  bool delta(int64_t tUs, Vec2 pos, float* deltaS);

  // Best trace for saving / loading (best/<course>.bin)
  const TracePoint* bestTrace() const { return best_; }
  int bestCount() const { return bestN_; }
  bool setBest(const TracePoint* pts, int n);

  float maxOffsetM = 10.0f;  // farther from the best lap's line: no delta
  int searchAhead = 75;      // trace points searched ahead of the last match (3 s at 25 Hz)
  float maxShiftS = 30.0f;   // when re-acquiring: only points within this time of now

 private:
  bool nearest(Vec2 p, int from, int to, float nowS, bool timeWindow, int* seg, float* u,
               float* d2) const;
  void push(int64_t tUs, Vec2 pos);

  TracePoint* best_ = nullptr;
  TracePoint* cur_ = nullptr;
  int bestN_ = 0;
  int curN_ = 0;
  int cap_ = 0;
  bool overflow_ = false;
  bool inLap_ = false;
  int64_t lapStartUs_ = 0;
  int match_ = 0;
  bool lost_ = true;
};

}  // namespace lt
