// test/common.h - shared setup for the host tests.
#pragma once

#include <cstdint>
#include <vector>

#include "core/geo.h"
#include "core/lap.h"
#include "synth.h"

namespace tcommon {

// Origin of the test plane: Sodegaura Forest Raceway (35.394 N, 140.089 E)
constexpr int32_t kLat0 = 353940000;
constexpr int32_t kLon0 = 1400890000;

inline lt::Plane plane() {
  lt::Plane pl;
  pl.init(kLat0, kLon0);
  return pl;
}

// Sector lines on the stadium: middle of the top straight, apex of the last corner.
inline double sectorS(const synth::Track& tr, int i) {
  const double pi = 3.14159265358979323846;
  return i == 0 ? tr.straight + pi * tr.radius + tr.straight / 2.0
                : 2.0 * tr.straight + pi * tr.radius + pi * tr.radius / 2.0;
}

inline lt::CourseDef stadiumCourse(const synth::Track& tr, const lt::Plane& pl, double sfS,
                                   int numSectors) {
  lt::CourseDef c{};
  c.startFinish = synth::gateAt(tr, pl, sfS, 30.0f);
  c.numSectors = numSectors;
  for (int i = 0; i < numSectors; ++i) c.sectors[i] = synth::gateAt(tr, pl, sectorS(tr, i), 30.0f);
  c.minLapS = 20.0f;
  return c;
}

struct Session {
  int starts = 0;
  std::vector<lt::LapResult> laps;
};

inline Session runTimer(lt::LapTimer& t, const std::vector<lt::Fix>& fixes) {
  Session s;
  for (const lt::Fix& f : fixes) {
    const uint32_t ev = t.feed(f);
    if (ev & lt::kEvLapStart) ++s.starts;
    if (ev & lt::kEvLap) s.laps.push_back(t.lastLap());
  }
  return s;
}

}  // namespace tcommon
