// tools/host/laps.cpp - runs core/lap on RaceChrono sessions and compares every lap with
// RaceChrono's own lap times (go.sh laps).
// The S/F line is put where RaceChrono crossed it: the positions at RaceChrono's lap
// boundaries are averaged, and the direction is the mean direction of travel there.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/geo.h"
#include "core/lap.h"
#include "tools/host/rcz.h"

namespace {

constexpr float kWidthNowM = 30.0f;       // S/F width in the current design
constexpr float kWidthProposedM = 50.0f;  // proposal: also counts the pit lane, like RaceChrono
constexpr float kMinLapS = 30.0f;
constexpr double kLimitS = 0.05;          // pass: every lap within +-0.05 s of RaceChrono

std::string lapText(double s) {
  char b[32];
  const int m = static_cast<int>(s / 60.0);
  std::snprintf(b, sizeof b, "%d:%06.3f", m, s - 60.0 * m);
  return b;
}

// Position and direction of travel at time tMs (linear between the two fixes around it)
bool posAt(const rcz::Session& s, const lt::Plane& pl, int64_t tMs, lt::Vec2* p, lt::Vec2* dir,
           double* speedKmh) {
  const auto it = std::upper_bound(s.tMs.begin(), s.tMs.end(), tMs);
  if (it == s.tMs.begin() || it == s.tMs.end()) return false;
  const size_t i = static_cast<size_t>(it - s.tMs.begin());
  const lt::Vec2 a = pl.toLocal(s.latE7[i - 1], s.lonE7[i - 1]);
  const lt::Vec2 b = pl.toLocal(s.latE7[i], s.lonE7[i]);
  const float f = static_cast<float>(tMs - s.tMs[i - 1]) / static_cast<float>(s.tMs[i] - s.tMs[i - 1]);
  *p = a + (b - a) * f;
  *dir = b - a;
  *speedKmh = static_cast<double>(s.speedMps[i]) * 3.6;
  return lt::length(*dir) > 0.0f;
}

struct Crossing {
  int64_t tMs;
  float alongM;    // distance from the derived line, along the travel direction
  float lateralM;  // position along the line (+ = right of the travel direction)
  double speedKmh;
};

struct Line {
  lt::GateDef def{};
  bool fromSpread = false;   // orientation fitted through the crossings (else: travel direction)
  std::vector<Crossing> rc;  // RaceChrono's crossings seen against this line
};

constexpr float kMinSpreadForFitM = 10.0f;  // crossings this far apart give the line's angle

Line deriveLine(const rcz::Session& s, const lt::Plane& pl) {
  std::vector<int64_t> times;
  for (const rcz::Lap& l : s.laps) {
    times.push_back(l.startMs);
    if (l.finished) times.push_back(l.finishMs);
  }
  std::sort(times.begin(), times.end());
  times.erase(std::unique(times.begin(), times.end()), times.end());

  std::vector<lt::Vec2> pts, dirs;
  std::vector<double> spd;
  std::vector<int64_t> used;
  for (int64_t t : times) {
    lt::Vec2 p{}, d{};
    double v = 0.0;
    if (!posAt(s, pl, t, &p, &d, &v)) continue;
    pts.push_back(p);
    dirs.push_back(d * (1.0f / lt::length(d)));
    spd.push_back(v);
    used.push_back(t);
  }
  Line line;
  if (pts.empty()) return line;
  lt::Vec2 c{0.0f, 0.0f}, h{0.0f, 0.0f};
  for (size_t i = 0; i < pts.size(); ++i) {
    c = c + pts[i];
    h = h + dirs[i];
  }
  c = c * (1.0f / static_cast<float>(pts.size()));
  h = h * (1.0f / lt::length(h));
  // RaceChrono's line need not be square to the travel direction. When the crossings are
  // spread along it (e.g. one in the pit lane), fit the line through them and use its normal.
  double sxx = 0.0, syy = 0.0, sxy = 0.0;
  for (const lt::Vec2& p : pts) {
    const lt::Vec2 d = p - c;
    sxx += static_cast<double>(d.x) * d.x;
    syy += static_cast<double>(d.y) * d.y;
    sxy += static_cast<double>(d.x) * d.y;
  }
  const double ang = 0.5 * std::atan2(2.0 * sxy, sxx - syy);  // direction of the line
  const lt::Vec2 along{static_cast<float>(std::cos(ang)), static_cast<float>(std::sin(ang))};
  float lo = 0.0f, hi = 0.0f;
  for (const lt::Vec2& p : pts) {
    lo = std::fmin(lo, lt::dot(p - c, along));
    hi = std::fmax(hi, lt::dot(p - c, along));
  }
  if (hi - lo >= kMinSpreadForFitM) {
    lt::Vec2 nrm{along.y, -along.x};
    if (lt::dot(nrm, h) < 0.0f) nrm = nrm * -1.0f;
    h = nrm;
    line.fromSpread = true;
  }
  const lt::Vec2 n{h.y, -h.x};
  pl.toGlobal(c, &line.def.latE7, &line.def.lonE7);
  line.def.headingDeg = lt::headingOf(h);
  for (size_t i = 0; i < pts.size(); ++i) {
    line.rc.push_back({used[i], lt::dot(pts[i] - c, h), lt::dot(pts[i] - c, n), spd[i]});
  }
  return line;
}

struct Row {
  int number;
  double rcS;
  double oursS;  // < 0: no matching lap
};

struct Result {
  std::vector<Row> rows;
  int rcLaps = 0;
  int ourLaps = 0;
  int matched = 0;
  double maxDiff = 0.0;
  bool pass() const { return matched == rcLaps && ourLaps == rcLaps && maxDiff <= kLimitS; }
};

Result run(const rcz::Session& s, const lt::Plane& pl, lt::GateDef gate, float widthM) {
  gate.widthM = widthM;
  lt::CourseDef c{};
  c.startFinish = gate;
  c.numSectors = 0;
  c.minLapS = kMinLapS;
  lt::LapTimer t;
  t.begin(pl, c);
  std::vector<lt::LapResult> ours;
  for (size_t i = 0; i < s.tMs.size(); ++i) {
    lt::Fix f{};
    f.tUs = s.tMs[i] * 1000;
    f.latE7 = s.latE7[i];
    f.lonE7 = s.lonE7[i];
    f.speedMps = s.speedMps[i];
    f.valid = true;
    if (t.feed(f) & lt::kEvLap) ours.push_back(t.lastLap());
  }
  Result r;
  r.ourLaps = static_cast<int>(ours.size());
  for (const rcz::Lap& l : s.laps) {
    if (!l.finished) continue;
    ++r.rcLaps;
    Row row{l.number, static_cast<double>(l.finishMs - l.startMs) * 1e-3, -1.0};
    for (const lt::LapResult& o : ours) {
      if (std::llabs(o.startUs / 1000 - l.startMs) <= 500 && std::llabs(o.endUs / 1000 - l.finishMs) <= 500) {
        row.oursS = static_cast<double>(o.lapUs) * 1e-6;
        ++r.matched;
        r.maxDiff = std::fmax(r.maxDiff, std::fabs(row.oursS - row.rcS));
      }
    }
    r.rows.push_back(row);
  }
  return r;
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
  if (argc < 2) {
    std::printf("usage: laps <unzipped .rcz directory>...\n");
    return 2;
  }
  bool allPass = true;
  for (int a = 1; a < argc; ++a) {
    rcz::Session s;
    std::string err;
    if (!rcz::load(argv[a], &s, &err)) {
      std::printf("FAIL %s: %s\n", argv[a], err.c_str());
      allPass = false;
      continue;
    }
    lt::Plane pl;
    pl.init(s.latE7[s.latE7.size() / 2], s.lonE7[s.lonE7.size() / 2]);
    const Line line = deriveLine(s, pl);
    const Result now = run(s, pl, line.def, kWidthNowM);
    const Result prop = run(s, pl, line.def, kWidthProposedM);

    std::printf("\n== %s\n", s.name.c_str());
    std::printf("track %s, GPS %.0f Hz, %zu fixes, RaceChrono laps %d\n", s.trackName.c_str(),
                rateHz(s), s.tMs.size(), prop.rcLaps);
    float aMin = 0.0f, aMax = 0.0f;
    for (const Crossing& c : line.rc) {
      aMin = std::fmin(aMin, c.alongM);
      aMax = std::fmax(aMax, c.alongM);
    }
    std::printf("S/F from RaceChrono's %zu crossings: %.7f N %.7f E, heading %.1f deg (%s);"
                " the crossings lie within %.2f m of this line\n",
                line.rc.size(), line.def.latE7 * 1e-7, line.def.lonE7 * 1e-7,
                static_cast<double>(line.def.headingDeg),
                line.fromSpread ? "square to the line through the crossings"
                                : "mean direction of travel",
                static_cast<double>(std::fmax(-aMin, aMax)));
    for (const Crossing& c : line.rc) {
      if (std::fabs(c.lateralM) > 0.5f * kWidthNowM) {
        std::printf("  crossing %.1f m beside the racing line at %.0f km/h (pit lane)\n",
                    static_cast<double>(c.lateralM), c.speedKmh);
      }
    }
    std::printf("lap   RaceChrono   S/F %2.0f m        S/F %2.0f m\n",
                static_cast<double>(kWidthProposedM), static_cast<double>(kWidthNowM));
    for (size_t i = 0; i < prop.rows.size(); ++i) {
      const Row& p = prop.rows[i];
      const Row& n = now.rows[i];
      std::string colP = p.oursS < 0 ? "not found      " : lapText(p.oursS);
      std::string colN = n.oursS < 0 ? "not found" : lapText(n.oursS);
      char dP[16] = "", dN[16] = "";
      if (p.oursS >= 0) std::snprintf(dP, sizeof dP, " %+.3f", p.oursS - p.rcS);
      if (n.oursS >= 0) std::snprintf(dN, sizeof dN, " %+.3f", n.oursS - n.rcS);
      std::printf("%3d  %10s   %s%-7s  %s%s\n", p.number, lapText(p.rcS).c_str(), colP.c_str(), dP,
                  colN.c_str(), dN);
    }
    std::printf("S/F %2.0f m: %d/%d laps match, our laps %d, max difference %.3f s -> %s\n",
                static_cast<double>(kWidthProposedM), prop.matched, prop.rcLaps, prop.ourLaps,
                prop.maxDiff, prop.pass() ? "PASS" : "FAIL");
    std::printf("S/F %2.0f m: %d/%d laps match, our laps %d, max difference %.3f s -> %s\n",
                static_cast<double>(kWidthNowM), now.matched, now.rcLaps, now.ourLaps, now.maxDiff,
                now.pass() ? "PASS" : "FAIL");
    allPass = allPass && prop.pass();
  }
  std::printf("\nRESULT: %s (judged with the proposed S/F width %.0f m, limit +-%.2f s)\n",
              allPass ? "PASS" : "FAIL", static_cast<double>(kWidthProposedM), kLimitS);
  return allPass ? 0 : 1;
}
