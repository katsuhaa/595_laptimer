// test/synth.h - synthetic GNSS runs on a stadium-shaped track with exact truth.
// Host only (tests and the test VBO). The same seed gives the same run on Mac and Linux.
#pragma once

#include <cstdint>
#include <vector>

#include "core/geo.h"
#include "core/lap.h"

namespace synth {

class Rng {  // xorshift64* + Box-Muller
 public:
  explicit Rng(uint64_t seed) : s_(seed ? seed : 1) {}
  double uniform();  // [0, 1)
  double normal();   // mean 0, sigma 1

 private:
  uint64_t s_;
  bool haveSpare_ = false;
  double spare_ = 0.0;
};

struct Pt {
  double x, y;  // local plane [m]
};

// Stadium: bottom straight driven east (+x), then counter-clockwise half circles.
struct Track {
  double straight = 800.0;    // each straight [m]
  double radius = 150.0;      // half circles [m]
  double cornerSpeed = 22.0;  // [m/s]
  double straightLo = 30.0;   // straight speed rises from Lo at the ends to Hi in the middle
  double straightHi = 45.0;

  double length() const;
  Pt at(double s) const;  // position at distance s (wraps every lap)
  double headingAt(double s) const;
  double speedAt(double s) const;
  bool inCorner(double s) const;
};

struct RunConfig {
  double rateHz = 25.0;
  double noiseM = 0.0;  // position noise sigma, correlated over about 0.4 s
  int laps = 5;         // timed laps; the run passes S/F laps + 1 times
  double sfS = 400.0;   // S/F position along the track [m]
  uint64_t seed = 1;
  double lapScaleMin = 0.97;  // per-lap time factor (1.03 = 3 % slower everywhere)
  double lapScaleMax = 1.03;
  int64_t t0Us = 1000000000;  // GNSS time at the start of the run
};

struct Run {
  std::vector<lt::Fix> fixes;
  std::vector<double> fixS;  // true distance of each fix
  std::vector<double> S;     // fine truth: distance at i * dt
  double dt = 0.001;
  double sfS = 0.0;
  double L = 0.0;
  int64_t t0Us = 0;

  double passS(int k) const { return k * L + sfS; }  // distance of the k-th S/F pass
  double timeAt(double s) const;                      // truth: when distance s was reached [s]
  int64_t timeAtUs(double s) const;                   // same, as GNSS time [us]
  double distAt(double t) const;
};

Run makeRun(const Track& tr, const lt::Plane& pl, const RunConfig& cfg);

// Line across the track at distance s, perpendicular to the direction of travel.
lt::GateDef gateAt(const Track& tr, const lt::Plane& pl, double s, float widthM);

}  // namespace synth
