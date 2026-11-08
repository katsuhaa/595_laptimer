// Tests of core/gtime. epoch/Clock/demoBase ported from the round gauge's pc_test/unit_main.cpp
// section 29 (katsuhaa/abarth595-round-gauge 05ab0a1); local time and session ids are new.
#include <cstring>

#include "core/gtime.h"
#include "core/ubx.h"
#include "tmini.h"

using lt::gtime::epoch;

TEST(gtime_epoch) {
  CHECK(epoch(1970, 1, 1, 0, 0, 0) == 0);
  CHECK(epoch(2024, 2, 29, 23, 59, 59) == 1709251199u);
  CHECK(epoch(2026, 10, 27, 1, 42, 18) == 1793065338u);
  CHECK(epoch(2100, 3, 1, 0, 0, 0) == 4107542400u);
  CHECK(epoch(2026, 12, 31, 23, 59, 60) == epoch(2027, 1, 1, 0, 0, 0));  // leap second as :60
}

TEST(gtime_clock_from_fixes) {
  lt::gtime::Clock c;
  uint32_t u = 0;
  CHECK(!c.valid() && !c.now(1000, &u));
  lt::GnssFix gf{};
  gf.timeOk = true;
  gf.year = 2026;
  gf.month = 10;
  gf.day = 27;
  gf.hour = 1;
  gf.min = 42;
  gf.sec = 18;
  gf.nano = -200000000;
  gf.rxMs = 50000;
  c.onFix(gf);
  // the second starts at millis 50200 (nano subtracted)
  CHECK(c.valid() && c.now(50199, &u) && u == 1793065337u && c.now(50200, &u) && u == 1793065338u &&
        c.now(51200, &u) && u == 1793065339u);
  CHECK(c.now(50200 + 3600000, &u) && u == 1793065338u + 3600);  // keeps going without the GNSS
  int64_t ms = 0;
  CHECK(c.nowUtcMs(50450, &ms) && ms == 1793065338000LL + 250);
  CHECK(c.nowUtcMs(49800, &ms) && ms == 1793065337600LL);
  gf.timeOk = false;
  gf.sec = 30;
  c.onFix(gf);
  CHECK(c.now(50200, &u) && u == 1793065338u);  // fixes without date/time are ignored
  gf.timeOk = true;
  gf.year = 2019;  // before 2020: ignored
  c.onFix(gf);
  CHECK(c.now(50200, &u) && u == 1793065338u);
  gf.year = 2026;
  gf.month = 13;
  c.onFix(gf);
  CHECK(c.now(50200, &u) && u == 1793065338u);
  c.reset();
  CHECK(!c.valid() && !c.now(50200, &u));
}

TEST(gtime_demo_base) {
  using lt::gtime::demoBase;
  CHECK(demoBase("Oct 27 2026", "10:42:18") == 1793065338u);  // JST 10:42:18 = UTC 1:42:18
  CHECK(demoBase("Jan  1 2027", "08:59:59") == epoch(2026, 12, 31, 23, 59, 59));
  CHECK(demoBase("Feb 29 2028", "23:00:00") == epoch(2028, 2, 29, 14, 0, 0));
  CHECK(demoBase("Foo 27 2026", "10:42:18") == 0 && demoBase("Oct 27 2026", "10-42-18") == 0 &&
        demoBase("Oct 27 2026", "25:00:00") == 0 && demoBase("Oct 27", "10:42:18") == 0 &&
        demoBase(nullptr, "10:42:18") == 0 && demoBase("Oct 27 2026", nullptr) == 0);
  CHECK(demoBase("Oct 27 2026", "10:42:18", 0) == epoch(2026, 10, 27, 10, 42, 18));
  CHECK(demoBase("Oct 27 2026", "10:42:18", -300) == epoch(2026, 10, 27, 15, 42, 18));
}

TEST(gtime_local_time_and_session_id) {
  lt::gtime::DateTime t;
  // UTC 2026-11-08 01:30:00 = JST 10:30:00
  lt::gtime::toLocal(epoch(2026, 11, 8, 1, 30, 0), lt::gtime::kTzJstMin, &t);
  CHECK(t.year == 2026 && t.month == 11 && t.day == 8 && t.hour == 10 && t.min == 30 && t.sec == 0 && t.wday == 0);
  char id[16];
  CHECK(lt::gtime::formatId(t, id, sizeof(id)) == 15 && std::strcmp(id, "20261108_103000") == 0);
  CHECK(lt::gtime::formatId(t, id, 15) == 0);
  // 15:00 UTC is midnight in Japan: the date moves on
  lt::gtime::toLocal(epoch(2026, 12, 31, 15, 0, 0), 540, &t);
  CHECK(t.year == 2027 && t.month == 1 && t.day == 1 && t.hour == 0 && t.min == 0 && t.wday == 5);
  lt::gtime::toLocal(epoch(2026, 12, 31, 14, 59, 59), 540, &t);
  CHECK(t.year == 2026 && t.month == 12 && t.day == 31 && t.hour == 23 && t.min == 59 && t.sec == 59);
  lt::gtime::toLocal(epoch(2028, 2, 28, 20, 0, 0), 540, &t);  // leap day
  CHECK(t.month == 2 && t.day == 29 && t.hour == 5);
  lt::gtime::toLocal(epoch(2027, 3, 1, 2, 0, 0), -300, &t);  // west of UTC: previous day
  CHECK(t.year == 2027 && t.month == 2 && t.day == 28 && t.hour == 21);
  lt::gtime::toLocal(0, 0, &t);
  CHECK(t.year == 1970 && t.month == 1 && t.day == 1 && t.hour == 0 && t.wday == 4);
  lt::gtime::toLocal(-1, 0, &t);
  CHECK(t.year == 1969 && t.month == 12 && t.day == 31 && t.hour == 23 && t.min == 59 && t.sec == 59 && t.wday == 3);
  // every day of 2026..2030 round-trips through epoch()
  int bad = 0;
  for (int64_t s = epoch(2026, 1, 1, 0, 0, 0); s < epoch(2031, 1, 1, 0, 0, 0); s += 86400 + 3601) {
    lt::gtime::toLocal(s, 0, &t);
    if (epoch(t.year, t.month, t.day, t.hour, t.min, t.sec) != static_cast<uint32_t>(s)) bad++;
  }
  CHECKF(bad == 0, "%d days do not round-trip", bad);
}
