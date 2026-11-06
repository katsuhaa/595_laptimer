#include <cmath>

#include "core/peakhold.h"
#include "tmini.h"

namespace {

// rpm at time t of a run: rise from `from` to `to` over riseMs, then `after`
struct Profile {
  float from, to, after;
  uint32_t riseMs;
  float at(uint32_t t) const {
    if (t >= riseMs) return t == riseMs ? to : after;
    return from + (to - from) * static_cast<float>(t) / static_cast<float>(riseMs);
  }
};

// Runs the profile at 25 Hz up to endMs and returns the peak hold at that moment
lt::PeakHold runTo(const Profile& p, uint32_t endMs) {
  lt::PeakHold h;
  for (uint32_t t = 0; t <= endMs; t += 40) h.update(true, p.at(t), t);
  return h;
}

bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

}  // namespace

TEST(peakhold_rise_hold_fall_like_round_gauge) {
  const Profile p = {2000.0f, 6000.0f, 4000.0f, 3000};  // 6000 at 3.0 s, then 4000
  lt::PeakHold h = runTo(p, 3000);
  CHECK(h.visible());
  CHECK(near(h.markerRpm(), 6000.0f, 1.0f));
  CHECK(near(h.topRpm(), 6000.0f, 1.0f));
  CHECK(!h.holding());  // accelerating: the number is the current rpm

  h = runTo(p, 3040);
  CHECK(h.holding());  // past the peak
  CHECK(near(h.markerRpm(), 6000.0f, 1.0f));

  h = runTo(p, 4480);  // within 1.5 s of the peak: the marker stays
  CHECK(near(h.markerRpm(), 6000.0f, 1.0f));
  h = runTo(p, 5000);  // 0.5 s of falling at 2000 rpm/s
  CHECKF(near(h.markerRpm(), 5000.0f, 81.0f), "marker %.0f", static_cast<double>(h.markerRpm()));
  h = runTo(p, 5600);  // reached the current rpm
  CHECK(near(h.markerRpm(), 4000.0f, 1.0f));
  CHECK(h.visible());
  CHECK(near(h.topRpm(), 6000.0f, 1.0f));  // the centre number keeps the highest value

  h = runTo(p, 7800);  // 5 s after the last clear rise (at 3.0 s): still shown
  CHECK(h.visible());
  h = runTo(p, 8200);  // then gone
  CHECK(!h.visible());
  CHECK(h.topRpm() < 0.0f);
}

TEST(peakhold_nothing_below_3000) {
  const Profile p = {1000.0f, 2900.0f, 2900.0f, 2000};
  const uint32_t ends[] = {1000u, 2000u, 4000u};
  for (uint32_t end : ends) {
    const lt::PeakHold h = runTo(p, end);
    CHECK(!h.visible());
    CHECK(h.topRpm() < 0.0f);
  }
}

TEST(peakhold_gone_when_marker_falls_below_3000) {
  const Profile p = {2000.0f, 5000.0f, 2000.0f, 2000};  // 5000 at 2.0 s, then 2000
  lt::PeakHold h = runTo(p, 4400);  // held to 3.5 s, falling: about 3200
  CHECKF(h.visible(), "marker %.0f", static_cast<double>(h.markerRpm()));
  h = runTo(p, 4600);  // about 2800: below 3000
  CHECK(!h.visible());
  CHECK(h.topRpm() < 0.0f);
}

TEST(peakhold_jitter_does_not_keep_it_shown) {
  lt::PeakHold h;
  uint32_t t = 0;
  for (; t <= 500; t += 40) h.update(true, 3000.0f + 2.0f * static_cast<float>(t), t);  // to 4000
  const uint32_t lastRise = t - 40;
  for (; t <= 12000; t += 40) h.update(true, 4000.0f + ((t / 40) % 2 ? 50.0f : -50.0f), t);
  CHECK(!h.visible());
  CHECKF(t > lastRise + 5000, "ran %u ms", static_cast<unsigned>(t));
}

TEST(peakhold_reset_when_rpm_signal_lost) {
  lt::PeakHold h = runTo({2000.0f, 6000.0f, 4000.0f, 3000}, 3200);
  CHECK(h.visible());
  h.update(false, 0.0f, 3240);
  CHECK(!h.visible());
  CHECK(h.topRpm() < 0.0f);
  h.update(true, 4000.0f, 3280);  // back: starts again from the current rpm
  CHECK(near(h.markerRpm(), 4000.0f, 1.0f));
  CHECK(!h.visible());
}
