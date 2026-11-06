#include <cmath>
#include <vector>

#include "common.h"
#include "core/ghost.h"
#include "core/lap.h"
#include "synth.h"
#include "tmini.h"

namespace {

double secondsOf(int64_t us) { return static_cast<double>(us) * 1e-6; }

struct GhostStats {
  int lapsChecked = 0;      // laps whose end delta was compared
  double maxEndErr = 0.0;   // |delta at the S/F crossing - (lap - previous best)|
  double maxRunErr = 0.0;   // |delta - truth| over every fix of laps 2..
  double sumSqRunErr = 0.0;
  double maxRunErrAtM = 0.0;  // distance into the lap where the largest error was
  int fixesWithDelta = 0;
  int fixesInLaterLaps = 0;
  int deltaInFirstLap = 0;  // must stay 0: no best lap yet
  double rms() const { return fixesWithDelta ? std::sqrt(sumSqRunErr / fixesWithDelta) : 0.0; }
};

// Lap timer + ghost driven the way the firmware will drive them.
GhostStats runGhost(double noise, uint64_t seed) {
  const synth::Track tr;
  const lt::Plane pl = tcommon::plane();
  synth::RunConfig cfg;
  cfg.noiseM = noise;
  cfg.seed = seed;
  cfg.laps = 6;
  const synth::Run run = synth::makeRun(tr, pl, cfg);

  lt::LapTimer timer;
  timer.begin(pl, tcommon::stadiumCourse(tr, pl, cfg.sfS, 0));
  static std::vector<lt::TracePoint> a(8000), b(8000);
  lt::Ghost ghost;
  ghost.begin(a.data(), b.data(), 8000);

  GhostStats st;
  int bestLapIdx = -1;  // synthetic lap index of the best lap (lap n = passes n-1 .. n)
  for (size_t i = 0; i < run.fixes.size(); ++i) {
    const lt::Fix& f = run.fixes[i];
    const int64_t bestBefore = timer.bestLapUs();
    const uint32_t ev = timer.feed(f);
    if (ev & lt::kEvLap) {
      float d = 0.0f;
      if (ghost.delta(timer.sfCrossUs(), timer.sfCrossPos(), &d)) {
        const double expect = secondsOf(timer.lastLap().lapUs - bestBefore);
        st.maxEndErr = std::fmax(st.maxEndErr, std::fabs(static_cast<double>(d) - expect));
        ++st.lapsChecked;
      }
      const bool best = (ev & lt::kEvBest) != 0;
      ghost.endLap(timer.sfCrossUs(), timer.sfCrossPos(), best);
      if (best) bestLapIdx = timer.lastLap().lapNo;
    }
    if (ev & (lt::kEvLap | lt::kEvLapStart)) ghost.startLap(timer.sfCrossUs(), timer.sfCrossPos());
    if (!timer.timing()) continue;

    const lt::Vec2 pos = pl.toLocal(f.latE7, f.lonE7);
    ghost.addSample(f.tUs, pos);
    float d = 0.0f;
    const bool have = ghost.delta(f.tUs, pos, &d);
    if (timer.currentLapNo() == 1) {
      if (have) ++st.deltaInFirstLap;
      continue;
    }
    ++st.fixesInLaterLaps;
    if (!have) continue;
    ++st.fixesWithDelta;
    // truth: elapsed now minus the best lap's elapsed at the same distance
    const int lapIdx = timer.currentLapNo();
    const double s = run.fixS[i];
    const double startS = run.passS(lapIdx - 1);
    const double into = s - startS;
    const double now = secondsOf(f.tUs) - secondsOf(run.timeAtUs(startS));
    const double bestStart = run.passS(bestLapIdx - 1);
    const double ghostT = secondsOf(run.timeAtUs(bestStart + into) - run.timeAtUs(bestStart));
    const double err = std::fabs(static_cast<double>(d) - (now - ghostT));
    st.sumSqRunErr += err * err;
    if (err > st.maxRunErr) {
      st.maxRunErr = err;
      st.maxRunErrAtM = into;
    }
  }
  return st;
}

}  // namespace

// Running delta: four position errors meet (current fix, best-lap point, both lap starts),
// each divided by the local speed. With 0.3 m at 22 m/s in the corners sigma is about 0.02 s,
// so single fixes reach 0.08 s; the limits are set to catch matching faults (jumps of seconds).
TEST(ghost_delta_follows_truth) {
  const double noises[] = {0.0, 0.3};
  for (double n : noises) {
    const GhostStats st = runGhost(n, 3);
    tmini::info("noise %.1f m: end-of-lap error max %.4f s over %d laps (limit 0.05)", n,
                st.maxEndErr, st.lapsChecked);
    tmini::info("noise %.1f m: running error rms %.4f s (limit 0.03), max %.4f s at %.0f m into"
                " the lap (limit 0.15), delta on %d/%d fixes",
                n, st.rms(), st.maxRunErr, st.maxRunErrAtM, st.fixesWithDelta,
                st.fixesInLaterLaps);
    CHECKF(st.lapsChecked == 5, "laps checked %d", st.lapsChecked);
    CHECKF(st.maxEndErr <= 0.05, "end error %.4f s", st.maxEndErr);
    CHECKF(st.rms() <= 0.03, "running error rms %.4f s", st.rms());
    CHECKF(st.maxRunErr <= 0.15, "running error max %.4f s", st.maxRunErr);
    CHECKF(st.fixesWithDelta >= st.fixesInLaterLaps * 99 / 100, "delta on %d of %d fixes",
           st.fixesWithDelta, st.fixesInLaterLaps);
    CHECK(st.deltaInFirstLap == 0);
  }
}

TEST(ghost_no_delta_off_the_line) {
  lt::TracePoint a[4], b[4];
  lt::Ghost g;
  g.begin(a, b, 4);
  g.startLap(0, {0.0f, 0.0f});
  g.addSample(5000000, {100.0f, 0.0f});
  g.endLap(10000000, {200.0f, 0.0f}, true);
  CHECK(g.hasBest());
  CHECK(std::fabs(g.bestLapS() - 10.0f) < 1e-4f);
  g.startLap(20000000, {0.0f, 0.0f});
  float d = 0.0f;
  CHECK(g.delta(26000000, {100.0f, 3.0f}, &d));  // 3 m to the side: still on the line
  CHECK(std::fabs(d - 1.0f) < 1e-3f);             // 6 s here vs 5 s on the best lap
  CHECK(!g.delta(27000000, {100.0f, 40.0f}, &d)); // 40 m away (pit lane)
}

TEST(ghost_overflowed_lap_not_kept) {
  lt::TracePoint a[3], b[3];
  lt::Ghost g;
  g.begin(a, b, 3);
  g.startLap(0, {0.0f, 0.0f});
  for (int i = 1; i <= 5; ++i) g.addSample(i * 1000000, {10.0f * static_cast<float>(i), 0.0f});
  g.endLap(6000000, {60.0f, 0.0f}, true);
  CHECK(!g.hasBest());
}
