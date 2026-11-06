// core/lap.h - lap and sector timing from GNSS fixes.
// A line is crossed when the segment between two consecutive fixes passes through it;
// the crossing time is interpolated linearly along that segment (GNSS time, not arrival time).
#pragma once

#include <cstdint>

#include "geo.h"

namespace lt {

// One GNSS epoch as the lap timer needs it.
struct Fix {
  int64_t tUs;     // GNSS time, monotonic [us]
  int32_t latE7;   // [1e-7 deg]
  int32_t lonE7;   // [1e-7 deg]
  float speedMps;  // ground speed reported by the receiver (Doppler) [m/s]
  bool valid;      // usable fix (3D, accuracy within limits); invalid = treated as missing
};

// A timing line: centre, direction of travel and width.
struct GateDef {
  int32_t latE7;
  int32_t lonE7;
  float headingDeg;  // direction of travel through the line (0 = north, clockwise)
  float widthM;      // full width of the line
};

constexpr int kMaxSectors = 8;  // sector lines between two S/F crossings

struct CourseDef {
  GateDef startFinish;
  GateDef sectors[kMaxSectors];  // in driving order
  int numSectors;
  float minLapS;  // shortest plausible lap; earlier S/F crossings are ignored
};

struct LapConfig {
  float minSpeedMps = 20.0f / 3.6f;  // slower: no line is evaluated (paddock, standstill)
  float maxGapS = 2.0f;              // no crossing across a data gap longer than this
  float maxJumpMps = 100.0f;         // implied speed above this = position glitch
};

// Event flags returned by LapTimer::feed()
constexpr uint32_t kEvLapStart = 1u << 0;  // first S/F crossing: timing starts
constexpr uint32_t kEvLap = 1u << 1;       // lap completed; the next starts at the same instant
constexpr uint32_t kEvBest = 1u << 2;      // the completed lap is the new best
constexpr uint32_t kEvSector = 1u << 3;    // a sector line was crossed

struct LapResult {
  int lapNo;        // 1-based
  int64_t startUs;  // S/F crossing that started the lap
  int64_t endUs;    // S/F crossing that ended it
  int64_t lapUs;
  int numSplits;    // numSectors + 1 when every sector line was crossed in order, else 0
  int64_t splitUs[kMaxSectors + 1];
};

class LapTimer {
 public:
  void begin(const Plane& plane, const CourseDef& course, const LapConfig& cfg = LapConfig());
  void reset();  // new session on the same course
  uint32_t feed(const Fix& f);

  bool timing() const { return lapStartUs_ >= 0; }
  int currentLapNo() const { return timing() ? completed_ + 1 : 0; }
  int completedLaps() const { return completed_; }
  int64_t lapStartUs() const { return lapStartUs_; }
  int64_t elapsedUs(int64_t nowUs) const { return timing() ? nowUs - lapStartUs_ : 0; }
  int sectorsDone() const { return nextSector_; }
  const LapResult& lastLap() const { return last_; }  // valid after kEvLap
  int64_t bestLapUs() const { return bestUs_; }       // -1: none yet
  int bestLapNo() const { return bestNo_; }
  // Last S/F crossing (valid after kEvLapStart / kEvLap)
  int64_t sfCrossUs() const { return sfCrossUs_; }
  Vec2 sfCrossPos() const { return sfCrossPos_; }

 private:
  struct Line {
    Vec2 c;  // centre
    Vec2 h;  // unit direction of travel
    Vec2 n;  // unit vector along the line
    float halfW;
  };
  static Line makeLine(const Plane& pl, const GateDef& d);
  static bool crosses(const Line& l, Vec2 p, Vec2 q, float* frac);
  uint32_t segment(Vec2 p, Vec2 q, int64_t t0, int64_t t1);
  uint32_t sector(Vec2 p, Vec2 q, int64_t t0, int64_t t1);
  uint32_t startFinish(Vec2 x, int64_t tx);
  void startLap(int64_t tUs);

  Plane plane_;
  LapConfig cfg_;
  Line sf_{};
  Line sec_[kMaxSectors]{};
  int numSec_ = 0;
  int64_t minLapUs_ = 0;

  bool havePrev_ = false;
  Fix prev_{};
  Vec2 prevPos_{0.0f, 0.0f};

  int64_t lapStartUs_ = -1;
  int completed_ = 0;
  int nextSector_ = 0;
  int64_t secCrossUs_[kMaxSectors]{};
  LapResult last_{};
  int64_t bestUs_ = -1;
  int bestNo_ = 0;
  int64_t sfCrossUs_ = -1;
  Vec2 sfCrossPos_{0.0f, 0.0f};
};

}  // namespace lt
