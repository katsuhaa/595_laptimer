// tools/host/laps.cpp - runs core/lap on RaceChrono sessions and compares every lap with
// RaceChrono's own lap times (go.sh laps).
// Course: RaceChrono's track definition ([laptiming] of a RaceChrono VBO export with the same
// track name, given with --course), or, without one, an S/F line placed where RaceChrono
// crossed it (positions at RaceChrono's lap boundaries).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/geo.h"
#include "core/lap.h"
#include "tools/host/rcz.h"
#include "tools/host/vboread.h"

namespace {

constexpr float kSfWidthM = 50.0f;     // decided 2026-11-07: the pit lane counts, as in RaceChrono
constexpr float kSplitWidthM = 30.0f;  // sector lines
constexpr float kMinLapS = 30.0f;
constexpr double kLimitS = 0.05;       // pass: every lap within +-0.05 s of RaceChrono

std::string lapText(double s) {
  char b[32];
  const int m = static_cast<int>(s / 60.0);
  std::snprintf(b, sizeof b, "%d:%06.3f", m, s - 60.0 * m);
  return b;
}

std::vector<lt::Fix> fixesOf(const rcz::Session& s) {
  std::vector<lt::Fix> v(s.tMs.size());
  for (size_t i = 0; i < s.tMs.size(); ++i) {
    v[i].tUs = s.tMs[i] * 1000;
    v[i].latE7 = s.latE7[i];
    v[i].lonE7 = s.lonE7[i];
    v[i].speedMps = s.speedMps[i];
    v[i].valid = true;
  }
  return v;
}

int32_t e7(double deg) { return static_cast<int32_t>(std::llround(deg * 1e7)); }

// RaceChrono line: through A, travelling from B towards A
lt::GateDef gateOf(const vboread::Gate& g, const lt::Plane& pl, float widthM) {
  const lt::Vec2 a = pl.toLocal(e7(g.latA), e7(g.lonA));
  const lt::Vec2 b = pl.toLocal(e7(g.latB), e7(g.lonB));
  return {e7(g.latA), e7(g.lonA), lt::headingOf(a - b), widthM};
}

// First forward crossing of `g` after `afterUs` (-1: none), using the lap timer itself
int64_t firstCrossing(const std::vector<lt::Fix>& fixes, const lt::Plane& pl, const lt::GateDef& g,
                      int64_t afterUs) {
  lt::CourseDef c{};
  c.startFinish = g;
  c.minLapS = kMinLapS;
  lt::LapTimer t;
  t.begin(pl, c);
  for (const lt::Fix& f : fixes) {
    if (f.tUs <= afterUs) continue;
    if (t.feed(f) & lt::kEvLapStart) return t.sfCrossUs();
  }
  return -1;
}

struct Course {
  lt::CourseDef def{};
  std::string source;
  std::vector<std::string> splitNames;
};

// RaceChrono's definition: S/F + split lines, the splits put in driving order
bool courseFromVbo(const std::vector<vboread::File>& vbos, const rcz::Session& s,
                   const std::vector<lt::Fix>& fixes, const lt::Plane& pl, Course* out) {
  for (const vboread::File& f : vbos) {
    if (f.sessionName != s.trackName) continue;
    const vboread::Gate* start = nullptr;
    std::vector<const vboread::Gate*> splits;
    for (const vboread::Gate& g : f.gates) {
      if (g.kind == "Start" && !start) start = &g;
      if (g.kind == "Split") splits.push_back(&g);
    }
    if (!start) continue;
    Course c;
    c.def.startFinish = gateOf(*start, pl, kSfWidthM);
    c.def.minLapS = kMinLapS;
    const int64_t t0 = firstCrossing(fixes, pl, c.def.startFinish, -1);
    std::vector<std::pair<int64_t, const vboread::Gate*>> order;
    for (const vboread::Gate* g : splits) {
      const int64_t t = firstCrossing(fixes, pl, gateOf(*g, pl, kSplitWidthM), t0);
      if (t >= 0) order.push_back({t, g});
    }
    std::sort(order.begin(), order.end(),
              [](const std::pair<int64_t, const vboread::Gate*>& x,
                 const std::pair<int64_t, const vboread::Gate*>& y) { return x.first < y.first; });
    for (const auto& o : order) {
      if (c.def.numSectors >= lt::kMaxSectors) break;
      c.def.sectors[c.def.numSectors++] = gateOf(*o.second, pl, kSplitWidthM);
      c.splitNames.push_back(o.second->name);
    }
    const size_t slash = f.path.find_last_of('/');
    c.source = "RaceChrono's track definition in " +
               (slash == std::string::npos ? f.path : f.path.substr(slash + 1));
    *out = c;
    return true;
  }
  return false;
}

// Without a definition: S/F where RaceChrono crossed it, square to the mean travel direction,
// or to the line through the crossings when they are spread out (one in the pit lane)
Course courseFromCrossings(const rcz::Session& s, const lt::Plane& pl) {
  std::vector<lt::Vec2> pts;
  lt::Vec2 h{0.0f, 0.0f};
  for (const rcz::Lap& l : s.laps) {
    for (int k = 0; k < (l.finished ? 2 : 1); ++k) {
      const int64_t t = k ? l.finishMs : l.startMs;
      const auto it = std::upper_bound(s.tMs.begin(), s.tMs.end(), t);
      if (it == s.tMs.begin() || it == s.tMs.end()) continue;
      const size_t i = static_cast<size_t>(it - s.tMs.begin());
      const lt::Vec2 a = pl.toLocal(s.latE7[i - 1], s.lonE7[i - 1]);
      const lt::Vec2 b = pl.toLocal(s.latE7[i], s.lonE7[i]);
      const float f = static_cast<float>(t - s.tMs[i - 1]) / static_cast<float>(s.tMs[i] - s.tMs[i - 1]);
      pts.push_back(a + (b - a) * f);
      if (lt::length(b - a) > 0.0f) h = h + (b - a) * (1.0f / lt::length(b - a));
    }
  }
  Course c;
  c.def.minLapS = kMinLapS;
  c.source = "line through RaceChrono's lap crossings";
  if (pts.empty() || lt::length(h) == 0.0f) return c;
  lt::Vec2 ctr{0.0f, 0.0f};
  for (const lt::Vec2& p : pts) ctr = ctr + p;
  ctr = ctr * (1.0f / static_cast<float>(pts.size()));
  h = h * (1.0f / lt::length(h));
  double sxx = 0.0, syy = 0.0, sxy = 0.0;
  for (const lt::Vec2& p : pts) {
    const lt::Vec2 d = p - ctr;
    sxx += static_cast<double>(d.x) * d.x;
    syy += static_cast<double>(d.y) * d.y;
    sxy += static_cast<double>(d.x) * d.y;
  }
  const double ang = 0.5 * std::atan2(2.0 * sxy, sxx - syy);
  const lt::Vec2 along{static_cast<float>(std::cos(ang)), static_cast<float>(std::sin(ang))};
  float lo = 0.0f, hi = 0.0f;
  for (const lt::Vec2& p : pts) {
    lo = std::fmin(lo, lt::dot(p - ctr, along));
    hi = std::fmax(hi, lt::dot(p - ctr, along));
  }
  if (hi - lo >= 10.0f) {
    lt::Vec2 nrm{along.y, -along.x};
    h = lt::dot(nrm, h) < 0.0f ? nrm * -1.0f : nrm;
  }
  pl.toGlobal(ctr, &c.def.startFinish.latE7, &c.def.startFinish.lonE7);
  c.def.startFinish.headingDeg = lt::headingOf(h);
  c.def.startFinish.widthM = kSfWidthM;
  return c;
}

// How far RaceChrono's lap crossings are from our S/F line (along the travel direction)
float worstOffsetM(const rcz::Session& s, const lt::Plane& pl, const lt::GateDef& g) {
  const lt::Vec2 c = pl.toLocal(g.latE7, g.lonE7);
  const lt::Vec2 h = lt::headingUnit(g.headingDeg);
  float worst = 0.0f;
  for (const rcz::Lap& l : s.laps) {
    for (int k = 0; k < (l.finished ? 2 : 1); ++k) {
      const int64_t t = k ? l.finishMs : l.startMs;
      const auto it = std::upper_bound(s.tMs.begin(), s.tMs.end(), t);
      if (it == s.tMs.begin() || it == s.tMs.end()) continue;
      const size_t i = static_cast<size_t>(it - s.tMs.begin());
      const lt::Vec2 a = pl.toLocal(s.latE7[i - 1], s.lonE7[i - 1]);
      const lt::Vec2 b = pl.toLocal(s.latE7[i], s.lonE7[i]);
      const float f = static_cast<float>(t - s.tMs[i - 1]) / static_cast<float>(s.tMs[i] - s.tMs[i - 1]);
      worst = std::fmax(worst, std::fabs(lt::dot(a + (b - a) * f - c, h)));
    }
  }
  return worst;
}

double rateHz(const rcz::Session& s) {
  std::vector<int64_t> dt;
  for (size_t i = 1; i < s.tMs.size(); ++i) dt.push_back(s.tMs[i] - s.tMs[i - 1]);
  if (dt.empty()) return 0.0;
  std::nth_element(dt.begin(), dt.begin() + static_cast<long>(dt.size() / 2), dt.end());
  return 1000.0 / static_cast<double>(dt[dt.size() / 2]);
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<vboread::File> vbos;
  std::vector<std::string> dirs;
  for (int a = 1; a < argc; ++a) {
    if (std::strcmp(argv[a], "--course") == 0 && a + 1 < argc) {
      vboread::File f;
      std::string err;
      if (vboread::read(argv[++a], &f, &err)) vbos.push_back(f);
      else std::printf("warning: %s\n", err.c_str());
    } else {
      dirs.push_back(argv[a]);
    }
  }
  if (dirs.empty()) {
    std::printf("usage: laps [--course <RaceChrono .vbo>]... <unzipped .rcz directory>...\n");
    return 2;
  }
  bool allPass = true;
  for (const std::string& dir : dirs) {
    rcz::Session s;
    std::string err;
    if (!rcz::load(dir, &s, &err)) {
      std::printf("FAIL %s: %s\n", dir.c_str(), err.c_str());
      allPass = false;
      continue;
    }
    lt::Plane pl;
    pl.init(s.latE7[s.latE7.size() / 2], s.lonE7[s.lonE7.size() / 2]);
    const std::vector<lt::Fix> fixes = fixesOf(s);
    Course course;
    if (!courseFromVbo(vbos, s, fixes, pl, &course)) course = courseFromCrossings(s, pl);
    const lt::GateDef& sf = course.def.startFinish;

    std::printf("\n== %s\n", s.name.c_str());
    std::printf("track %s, GPS %.0f Hz, %zu fixes\n", s.trackName.c_str(), rateHz(s), s.tMs.size());
    std::printf("course: %s\n", course.source.c_str());
    std::printf("  S/F      %.7f N %.7f E heading %5.1f deg, %2.0f m wide;"
                " RaceChrono's crossings within %.2f m of it\n",
                sf.latE7 * 1e-7, sf.lonE7 * 1e-7, static_cast<double>(sf.headingDeg),
                static_cast<double>(sf.widthM), static_cast<double>(worstOffsetM(s, pl, sf)));
    for (int i = 0; i < course.def.numSectors; ++i) {
      const lt::GateDef& g = course.def.sectors[i];
      std::printf("  %-8s %.7f N %.7f E heading %5.1f deg, %2.0f m wide\n",
                  course.splitNames[static_cast<size_t>(i)].c_str(), g.latE7 * 1e-7,
                  g.lonE7 * 1e-7, static_cast<double>(g.headingDeg), static_cast<double>(g.widthM));
    }

    lt::LapTimer t;
    t.begin(pl, course.def);
    std::vector<lt::LapResult> ours;
    for (const lt::Fix& f : fixes) {
      if (t.feed(f) & lt::kEvLap) ours.push_back(t.lastLap());
    }
    std::printf("lap   RaceChrono   this        diff   sectors\n");
    int rcLaps = 0, matched = 0;
    double maxDiff = 0.0;
    for (const rcz::Lap& l : s.laps) {
      if (!l.finished) continue;
      ++rcLaps;
      const double rc = static_cast<double>(l.finishMs - l.startMs) * 1e-3;
      const lt::LapResult* m = nullptr;
      for (const lt::LapResult& o : ours) {
        if (std::llabs(o.startUs / 1000 - l.startMs) <= 500 && std::llabs(o.endUs / 1000 - l.finishMs) <= 500) {
          m = &o;
        }
      }
      if (!m) {
        std::printf("%3d  %10s   not found\n", l.number, lapText(rc).c_str());
        continue;
      }
      ++matched;
      const double ourS = static_cast<double>(m->lapUs) * 1e-6;
      maxDiff = std::fmax(maxDiff, std::fabs(ourS - rc));
      std::string sec;
      for (int k = 0; k < m->numSplits; ++k) {
        char b[16];
        std::snprintf(b, sizeof b, " %6.3f", static_cast<double>(m->splitUs[k]) * 1e-6);
        sec += b;
      }
      if (course.def.numSectors > 0 && m->numSplits == 0) sec = " (a sector line was missed)";
      std::printf("%3d  %10s   %s  %+.3f %s\n", l.number, lapText(rc).c_str(), lapText(ourS).c_str(),
                  ourS - rc, sec.c_str());
    }
    const bool pass = matched == rcLaps && static_cast<int>(ours.size()) == rcLaps && maxDiff <= kLimitS;
    std::printf("%d/%d laps match RaceChrono, our laps %zu, max difference %.3f s -> %s\n", matched,
                rcLaps, ours.size(), maxDiff, pass ? "PASS" : "FAIL");
    allPass = allPass && pass;
  }
  std::printf("\nRESULT: %s (limit +-%.2f s)\n", allPass ? "PASS" : "FAIL", kLimitS);
  return allPass ? 0 : 1;
}
