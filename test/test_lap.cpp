#include <cmath>
#include <vector>

#include "common.h"
#include "core/lap.h"
#include "synth.h"
#include "tmini.h"

using tcommon::kLat0;
using tcommon::kLon0;

namespace {

double secondsOf(int64_t us) { return static_cast<double>(us) * 1e-6; }

// ---- synthetic laps with exact truth -------------------------------------------------

struct Accuracy {
  int laps = 0;
  double maxLapErr = 0.0;
  double maxSplitErr = 0.0;
  bool splitsComplete = true;
};

Accuracy measure(double hz, double noise, uint64_t seed, int numSectors) {
  const synth::Track tr;
  const lt::Plane pl = tcommon::plane();
  synth::RunConfig cfg;
  cfg.rateHz = hz;
  cfg.noiseM = noise;
  cfg.seed = seed;
  const synth::Run run = synth::makeRun(tr, pl, cfg);
  lt::LapTimer t;
  t.begin(pl, tcommon::stadiumCourse(tr, pl, cfg.sfS, numSectors));
  const tcommon::Session s = tcommon::runTimer(t, run.fixes);

  Accuracy a;
  a.laps = static_cast<int>(s.laps.size());
  for (size_t i = 0; i < s.laps.size(); ++i) {
    const lt::LapResult& r = s.laps[i];
    const double lapStartS = run.passS(static_cast<int>(i));
    const double truth = secondsOf(run.timeAtUs(lapStartS + run.L) - run.timeAtUs(lapStartS));
    a.maxLapErr = std::fmax(a.maxLapErr, std::fabs(secondsOf(r.lapUs) - truth));
    if (r.numSplits != numSectors + 1) {
      a.splitsComplete = false;
      continue;
    }
    double from = lapStartS;
    for (int k = 0; k <= numSectors; ++k) {
      const double to = k < numSectors ? lapStartS + (tcommon::sectorS(tr, k) - cfg.sfS)
                                       : lapStartS + run.L;
      const double truthSplit = secondsOf(run.timeAtUs(to) - run.timeAtUs(from));
      a.maxSplitErr = std::fmax(a.maxSplitErr, std::fabs(secondsOf(r.splitUs[k]) - truthSplit));
      from = to;
    }
  }
  return a;
}

// ---- straight passes through a test line at the origin ------------------------------

const lt::GateDef kLine = {kLat0, kLon0, 270.0f, 30.0f};  // travel west, 30 m wide

lt::CourseDef lineCourse() {
  lt::CourseDef c{};
  c.startFinish = kLine;
  c.numSectors = 0;
  c.minLapS = 20.0f;
  return c;
}

// Drive along y = yOff from x = +100 to x = -100 (forward = true, westward through the line)
// or back, 25 Hz at constant speed. Fixes with gapFrom < t < gapTo are left out.
void pass(std::vector<lt::Fix>& out, double yOff, bool forward, double t0, double speed,
          double gapFrom = 1e18, double gapTo = -1e18) {
  const lt::Plane pl = tcommon::plane();
  const double step = speed * 0.04;
  for (int k = 0; k * step <= 200.0; ++k) {
    const double t = t0 + 0.04 * k;
    if (t > gapFrom && t < gapTo) continue;
    const double x = forward ? 100.0 - k * step : -100.0 + k * step;
    lt::Fix f{};
    pl.toGlobal({static_cast<float>(x), static_cast<float>(yOff)}, &f.latE7, &f.lonE7);
    f.tUs = std::llround(t * 1e6);
    f.speedMps = static_cast<float>(speed);
    f.valid = true;
    out.push_back(f);
  }
}

// Line crossing time of a forward pass that started at t0
double crossT(double t0, double speed) { return t0 + 100.0 / speed; }

tcommon::Session runLine(const std::vector<lt::Fix>& fixes) {
  lt::LapTimer t;
  t.begin(tcommon::plane(), lineCourse());
  return tcommon::runTimer(t, fixes);
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

}  // namespace

// Each line crossing is off by about (position error along the track) / (speed at the line);
// a lap or split time is the difference of two crossings. Limit = 3 sigma of that + 1 ms.
double limitFor(double noise, double speedA, double speedB) {
  const double a = noise / speedA;
  const double b = noise / speedB;
  return 3.0 * std::sqrt(a * a + b * b) + 0.001;
}

TEST(lap_accuracy_synthetic) {
  const synth::Track tr;
  const double vSf = tr.speedAt(400.0);           // S/F: middle of the bottom straight
  const double vSlow = tr.speedAt(tcommon::sectorS(tr, 1));  // sector 2: corner apex
  const double cases[][2] = {{25, 0.0}, {25, 0.3}, {25, 1.0}, {10, 0.3}, {10, 1.0}};
  tmini::info("stadium %.0f m, 5 laps, lap-to-lap speed +-3 %%, 2 sectors,"
              " line speeds %.0f / %.0f m/s",
              tr.length(), vSf, vSlow);
  for (const auto& c : cases) {
    const double hz = c[0];
    const double noise = c[1];
    const double limLap = limitFor(noise, vSf, vSf);
    const double limSplit = limitFor(noise, vSlow, vSf);
    const Accuracy a = measure(hz, noise, 1, 2);
    tmini::info("%2.0f Hz noise %.1f m: laps %d/5  lap error max %.4f s (limit %.4f)"
                "  split error max %.4f s (limit %.4f)",
                hz, noise, a.laps, a.maxLapErr, limLap, a.maxSplitErr, limSplit);
    CHECKF(a.laps == 5, "%.0f Hz %.1f m: %d laps", hz, noise, a.laps);
    CHECKF(a.splitsComplete, "%.0f Hz %.1f m: splits missing", hz, noise);
    CHECKF(a.maxLapErr <= limLap, "%.0f Hz %.1f m: lap error %.4f s", hz, noise, a.maxLapErr);
    CHECKF(a.maxSplitErr <= limSplit, "%.0f Hz %.1f m: split error %.4f s", hz, noise,
           a.maxSplitErr);
  }
}

TEST(lap_accuracy_seeds_25hz_0_3m) {
  // the same condition with 10 other random runs, to see the spread
  const synth::Track tr;
  const double vSf = tr.speedAt(400.0);
  double worst = 0.0;
  for (uint64_t seed = 2; seed <= 11; ++seed) {
    worst = std::fmax(worst, measure(25, 0.3, seed, 0).maxLapErr);
  }
  const double lim = limitFor(0.3, vSf, vSf);
  tmini::info("10 runs x 5 laps, 25 Hz, noise 0.3 m: worst lap error %.4f s (limit %.4f),"
              " expected sigma %.4f s",
              worst, lim, std::sqrt(2.0) * 0.3 / vSf);
  CHECKF(worst <= lim, "worst %.4f s", worst);
}

TEST(lap_parallel_road_reverse_and_1s_dropout) {
  const double v = 37.5;
  std::vector<lt::Fix> f;
  pass(f, 0, true, 0, v);                                            // start
  pass(f, 0, true, 30, v);                                           // lap 1: 30.000
  pass(f, 40, true, 60, v);                                          // parallel road 40 m away
  pass(f, 0, false, 90, v);                                          // reverse
  pass(f, 0, true, 120, v, crossT(120, v) - 0.5, crossT(120, v) + 0.5);  // 1 s dropout on the line
  const tcommon::Session s = runLine(f);
  CHECK(s.starts == 1);
  CHECKF(s.laps.size() == 2, "laps %zu", s.laps.size());
  if (s.laps.size() == 2) {
    tmini::info("laps %.4f %.4f s (expected 30.0000 90.0000)", secondsOf(s.laps[0].lapUs),
                secondsOf(s.laps[1].lapUs));
    CHECK(near(secondsOf(s.laps[0].lapUs), 30.0, 0.001));
    CHECK(near(secondsOf(s.laps[1].lapUs), 90.0, 0.001));
  }
}

TEST(lap_gap_over_2s_not_bridged) {
  const double v = 37.5;
  std::vector<lt::Fix> f;
  pass(f, 0, true, 0, v);
  pass(f, 0, true, 30, v, crossT(30, v) - 1.5, crossT(30, v) + 1.5);  // 3 s without fixes
  pass(f, 0, true, 60, v);
  const tcommon::Session s = runLine(f);
  CHECKF(s.laps.size() == 1, "laps %zu", s.laps.size());
  if (s.laps.size() == 1) CHECK(near(secondsOf(s.laps[0].lapUs), 60.0, 0.001));
}

TEST(lap_position_glitch_ignored) {
  const double v = 37.5;
  std::vector<lt::Fix> f;
  pass(f, 0, true, 0, v);
  std::vector<lt::Fix> g;
  pass(g, 0, true, 30, v);
  // one fix 1 s into the pass jumps 50 m beyond the line and comes back
  const lt::Plane pl = tcommon::plane();
  pl.toGlobal({-50.0f, 0.0f}, &g[25].latE7, &g[25].lonE7);
  f.insert(f.end(), g.begin(), g.end());
  const tcommon::Session s = runLine(f);
  CHECKF(s.laps.size() == 1, "laps %zu", s.laps.size());
  if (s.laps.size() == 1) CHECK(near(secondsOf(s.laps[0].lapUs), 30.0, 0.001));
}

TEST(lap_standstill_on_the_line_ignored) {
  const double v = 37.5;
  std::vector<lt::Fix> f;
  pass(f, 0, true, 0, v);
  const lt::Plane pl = tcommon::plane();
  for (int k = 0; k < 250; ++k) {  // 10 s parked on the line, position jumping +-1 m
    lt::Fix x{};
    pl.toGlobal({k % 2 ? -1.0f : 1.0f, 0.0f}, &x.latE7, &x.lonE7);
    x.tUs = std::llround((10.0 + 0.04 * k) * 1e6);
    x.speedMps = 0.3f;
    x.valid = true;
    f.push_back(x);
  }
  pass(f, 0, true, 40, v);
  const tcommon::Session s = runLine(f);
  CHECKF(s.laps.size() == 1, "laps %zu", s.laps.size());
  if (s.laps.size() == 1) CHECK(near(secondsOf(s.laps[0].lapUs), 40.0, 0.001));
}

TEST(lap_shorter_than_min_lap_ignored) {
  const double v = 37.5;
  std::vector<lt::Fix> f;
  pass(f, 0, true, 0, v);
  pass(f, 0, true, 10, v);  // 10 s later: below the 20 s minimum
  pass(f, 0, true, 40, v);
  const tcommon::Session s = runLine(f);
  CHECKF(s.laps.size() == 1, "laps %zu", s.laps.size());
  if (s.laps.size() == 1) CHECK(near(secondsOf(s.laps[0].lapUs), 40.0, 0.001));
}

TEST(lap_fix_exactly_on_line_counted_once) {
  const double v = 25.0;  // 1.0 m per fix: one fix lands exactly on the line
  std::vector<lt::Fix> f;
  pass(f, 0, true, 0, v);
  pass(f, 0, true, 30, v);
  const tcommon::Session s = runLine(f);
  CHECK(s.starts == 1);
  CHECKF(s.laps.size() == 1, "laps %zu", s.laps.size());
  if (s.laps.size() == 1) CHECK(near(secondsOf(s.laps[0].lapUs), 30.0, 0.0005));
}

TEST(lap_missing_sector_keeps_lap) {
  const synth::Track tr;
  const lt::Plane pl = tcommon::plane();
  synth::RunConfig cfg;
  synth::Run run = synth::makeRun(tr, pl, cfg);
  // lap 2: no fixes for 3 s around the first sector line
  const double secPass = run.passS(1) + (tcommon::sectorS(tr, 0) - cfg.sfS);
  const int64_t tSec = run.timeAtUs(secPass);
  for (lt::Fix& x : run.fixes) {
    if (x.tUs > tSec - 1500000 && x.tUs < tSec + 1500000) x.valid = false;
  }
  lt::LapTimer t;
  t.begin(pl, tcommon::stadiumCourse(tr, pl, cfg.sfS, 2));
  const tcommon::Session s = tcommon::runTimer(t, run.fixes);
  CHECKF(s.laps.size() == 5, "laps %zu", s.laps.size());
  for (size_t i = 0; i < s.laps.size(); ++i) {
    const int expect = i == 1 ? 0 : 3;
    CHECKF(s.laps[i].numSplits == expect, "lap %zu splits %d", i + 1, s.laps[i].numSplits);
    if (s.laps[i].numSplits == 3) {
      CHECK(s.laps[i].splitUs[0] + s.laps[i].splitUs[1] + s.laps[i].splitUs[2] == s.laps[i].lapUs);
    }
  }
}

TEST(lap_best_lap_tracking) {
  const double v = 37.5;
  std::vector<lt::Fix> f;
  pass(f, 0, true, 0, v);
  pass(f, 0, true, 32, v);  // 32
  pass(f, 0, true, 62, v);  // 30 best
  pass(f, 0, true, 93, v);  // 31
  lt::LapTimer t;
  t.begin(tcommon::plane(), lineCourse());
  int bestEvents = 0;
  for (const lt::Fix& x : f) {
    if (t.feed(x) & lt::kEvBest) ++bestEvents;
  }
  CHECK(t.completedLaps() == 3);
  CHECK(t.bestLapNo() == 2);
  CHECK(near(secondsOf(t.bestLapUs()), 30.0, 0.001));
  CHECK(bestEvents == 2);
}
