// Tests of core/odolog (走行記録) and the odometer in core/veh. Ported from the round gauge's
// pc_test/unit_main.cpp section "2.15.0: 走行記録" (katsuhaa/abarth595-round-gauge d64851b): same frames,
// same steps, same expected values (in.aboveMs plays the round gauge's trip::aboveMs).
#include <cstring>
#include <vector>

#include "core/gtime.h"
#include "core/odolog.h"
#include "core/veh.h"
#include "tmini.h"

namespace {

using lt::OdoLog;

// The round gauge's host storage: a RAM area that behaves like flash (bits only 1 -> 0, erase = 0xFF)
struct RamFlash : lt::OdoStorage {
  std::vector<uint8_t> m;
  bool writeOk = true;
  explicit RamFlash(uint32_t n = lt::ODO_AREA_BYTES) : m(n, 0xFF) {}
  uint32_t size() override { return static_cast<uint32_t>(m.size()); }
  bool read(uint32_t off, void* d, size_t n) override {
    if (off + n > m.size()) return false;
    std::memcpy(d, m.data() + off, n);
    return true;
  }
  bool write(uint32_t off, const void* d, size_t n) override {
    if (!writeOk || off + n > m.size()) return false;
    for (size_t i = 0; i < n; i++) m[off + i] &= static_cast<const uint8_t*>(d)[i];
    return true;
  }
  bool erase(uint32_t off, size_t n) override {
    if (off % 4096 || n % 4096 || off + n > m.size()) return false;
    std::memset(m.data() + off, 0xFF, n);
    return true;
  }
  void clear() { std::fill(m.begin(), m.end(), 0xFF); }
};

std::vector<uint8_t> g_table(OdoLog::kTableBytes);

}  // namespace

TEST(veh_odometer) {
  // B-CAN 0C014003: b1 low 4 bits + b2 + b3. Frame of the 10/22 log 00 00 59 96 81 A9 00 00 = 22934 km
  lt::VehDecoder v;
  uint32_t t = 1000;
  lt::OdoInfo oi;
  v.odoInfo(&oi, t);
  CHECK(!oi.have && !oi.seen);
  uint8_t fr[8] = {0x00, 0x00, 0x59, 0x96, 0x81, 0xA9, 0x00, 0x00};
  v.onFrame(1, 1, lt::VEH_ID_ODO, 8, fr, t);
  v.odoInfo(&oi, t);
  CHECKF(oi.have && oi.seen && oi.km == 22934, "km %u", oi.km);
  v.onFrame(0, 1, lt::VEH_ID_ODO, 8, fr, t);  // the same id on C-CAN is not the odometer
  fr[1] = 0x50;  // the upper 4 bits are not used
  fr[3] = 0x97;
  v.onFrame(1, 1, lt::VEH_ID_ODO, 8, fr, t += 100);
  v.odoInfo(&oi, t);
  CHECK(oi.km == 22935);
  fr[3] = 0xA9;  // +18 km
  v.onFrame(1, 1, lt::VEH_ID_ODO, 8, fr, t += 100);
  fr[3] = 0x90;  // smaller
  v.onFrame(1, 1, lt::VEH_ID_ODO, 8, fr, t += 100);
  v.odoInfo(&oi, t);
  CHECKF(oi.km == 22935 && oi.rejected == 2, "km %u rejected %u", oi.km, oi.rejected);
  fr[3] = 0xA9;
  for (int i = 0; i < lt::ODO_ACCEPT_REPEAT; i++) v.onFrame(1, 1, lt::VEH_ID_ODO, 8, fr, t += 100);
  v.odoInfo(&oi, t);
  CHECKF(oi.km == 22953, "a dropped value that comes %d times is used: %u", lt::ODO_ACCEPT_REPEAT, oi.km);
  v.odoInfo(&oi, t + lt::ODO_STALE_MS + 1);
  CHECK(oi.have && !oi.seen);
}

TEST(odolog_dates) {
  int y, m, dd, w;
  OdoLog::civil(0, &y, &m, &dd, &w);
  CHECK(y == 1970 && m == 1 && dd == 1 && w == 4);
  const int32_t nov8 = OdoLog::daysFrom(2026, 11, 8);
  OdoLog::civil(nov8, &y, &m, &dd, &w);
  CHECK(y == 2026 && m == 11 && dd == 8 && w == 0);  // a Sunday
  OdoLog::civil(OdoLog::daysFrom(2028, 2, 29), &y, &m, &dd, &w);
  CHECK(y == 2028 && m == 2 && dd == 29);
  CHECK(OdoLog::daysFrom(2027, 1, 1) - OdoLog::daysFrom(2026, 12, 31) == 1);
  CHECK(OdoLog::dayOfUtc(lt::gtime::epoch(2026, 11, 7, 15, 0, 0)) == nov8);  // 0:00 JST
  CHECK(OdoLog::dayOfUtc(lt::gtime::epoch(2026, 11, 7, 14, 59, 59)) == nov8 - 1);
}

TEST(odolog_write_rule) {
  static RamFlash fl;
  static OdoLog o;
  fl.clear();
  o.begin(&fl, g_table.data());
  OdoLog::Status st;
  o.status(&st);
  CHECKF(st.storageOk && st.records == 0 && st.capacity == 28672, "capacity %u", st.capacity);
  const int32_t nov8 = OdoLog::daysFrom(2026, 11, 8);
  const uint32_t utc0 = lt::gtime::epoch(2026, 11, 8, 1, 0, 0);  // 10:00 JST
  uint32_t to = 5000;
  OdoLog::Input in;
  std::memset(&in, 0, sizeof(in));
  in.odoOk = in.odoHave = true;
  in.odoKm = 23500;
  in.speedOk = true;
  in.speedKmh = 0;
  auto run = [&](uint32_t ms) {  // update every 100 ms (the time goes on with it)
    for (uint32_t e = 0; e < ms; e += 100) {
      to += 100;
      if (in.timeOk) in.utc = utc0 + (to - 5000) / 1000;
      o.update(in, to);
    }
  };
  run(5000);
  o.status(&st);
  CHECK(st.writes == 0 && !st.timeKnown);  // no time: nothing written
  in.timeOk = true;
  run(5000);
  o.status(&st);
  CHECK(st.writes == 0 && st.timeKnown && st.today == nov8);  // nothing driven
  in.speedKmh = 50;
  in.odoKm = 23510;
  in.aboveMs = 120000;
  run(2000);
  o.status(&st);
  CHECKF(st.writes == 0 && st.todayKm == 10 && st.todayAboveS == 120, "moving: counted, not written (%u km %u s)",
         st.todayKm, st.todayAboveS);
  in.speedKmh = 12;
  run(1000);
  o.status(&st);
  CHECK(st.writes == 0);  // 12 km/h is faster than 10
  in.speedKmh = 8;
  run(100);
  o.status(&st);
  CHECKF(st.writes == 1 && st.records == 1 && st.lastKm == 10 && st.lastAboveS == 120 && st.lastOdo == 23510 &&
             st.lastReason == OdoLog::R_LOW,
         "first record: writes %u km %u s %u reason %u", st.writes, st.lastKm, st.lastAboveS, st.lastReason);
  in.speedKmh = 0;
  in.odoKm = 23511;
  run(30000);
  o.status(&st);
  CHECK(st.writes == 1);  // within ODO_MIN_INTERVAL_MS
  run(31000);
  o.status(&st);
  CHECK(st.writes == 2 && st.lastKm == 11 && st.lastReason == OdoLog::R_STILL);
  run(120000);
  o.status(&st);
  CHECK(st.writes == 2);  // nothing grew
  in.speedKmh = 20;
  in.reverse = true;
  in.aboveMs = 125000;
  run(100);
  o.status(&st);
  CHECKF(st.writes == 3 && st.lastAboveS == 125 && st.lastReason == OdoLog::R_REVERSE, "reverse at 20 km/h: %u %u %u",
         st.writes, st.lastAboveS, st.lastReason);
  in.reverse = false;
  in.speedKmh = 0;

  // power off and on, same day: today's start odometer and over-rpm time continue
  o.begin(&fl, g_table.data());
  in.timeOk = false;
  o.update(in, to += 100);
  o.status(&st);
  CHECK(st.records == 3 && st.lastOdo == 23511 && !st.timeKnown);
  in.timeOk = true;
  in.odoKm = 23513;
  run(200);
  o.status(&st);
  CHECKF(st.todayKm == 13 && st.todayAboveS == 125, "%u km %u s", st.todayKm, st.todayAboveS);
  uint32_t km = 0, ab = 0;
  CHECK(o.day(nov8, &km, &ab) && km == 13 && ab == 125);
  CHECK(!o.day(nov8 - 1, &km, &ab) && km == 0);
  OdoLog::Period pd;
  CHECK(o.periodAt(OdoLog::LV_WEEK, nov8, &pd) && pd.first == nov8 && pd.last == nov8 + 6 && pd.km == 13 &&
        pd.odoEnd == 23513 && pd.odoStart == 23500 && !pd.canBack && !pd.canFwd && pd.current);
  CHECK(o.periodAt(OdoLog::LV_MONTH, nov8, &pd) && pd.first == OdoLog::daysFrom(2026, 11, 1) &&
        pd.last == OdoLog::daysFrom(2026, 11, 30) && pd.km == 13);
  CHECK(o.periodAt(OdoLog::LV_QUARTER, nov8, &pd) && pd.first == OdoLog::daysFrom(2026, 10, 1) &&
        pd.last == OdoLog::daysFrom(2026, 12, 31));
  CHECK(o.periodAt(OdoLog::LV_QUARTER, OdoLog::daysFrom(2026, 2, 14), &pd) && pd.first == OdoLog::daysFrom(2026, 1, 1) &&
        pd.last == OdoLog::daysFrom(2026, 3, 31) && !pd.odoKnown);
  CHECK(o.periodAt(OdoLog::LV_YEAR, nov8, &pd) && pd.first == OdoLog::daysFrom(2026, 1, 1) &&
        pd.last == OdoLog::daysFrom(2026, 12, 31) && pd.current && pd.km == 13);
  CHECK(OdoLog::prevAnchor(OdoLog::LV_YEAR, nov8) == OdoLog::daysFrom(2025, 12, 31) &&
        OdoLog::prevAnchor(OdoLog::LV_MONTH, OdoLog::daysFrom(2026, 1, 20)) == OdoLog::daysFrom(2025, 12, 31) &&
        o.nextAnchor(OdoLog::LV_QUARTER, OdoLog::daysFrom(2026, 9, 3)) == nov8);

  // midnight while driving: the previous day's close is written the next time writing is allowed
  in.speedKmh = 40;
  in.odoKm = 23520;
  in.utc = utc0 + 14 * 3600 - 1;  // 11/8 23:59:59 JST
  o.update(in, to);
  in.odoKm = 23521;
  in.utc = utc0 + 14 * 3600 + 1;  // 11/9 0:00:01
  o.update(in, to += 2000);
  o.status(&st);
  const uint32_t w0 = st.writes;
  CHECK(st.today == nov8 + 1 && st.todayKm == 1);
  CHECK(o.day(nov8, &km, &ab) && km == 20);
  in.speedKmh = 5;
  in.odoKm = 23522;
  o.update(in, to += 100);
  o.status(&st);
  CHECK(st.writes == w0 + 1 && st.lastReason == OdoLog::R_CLOSE && st.lastKm == 20);  // no interval wait
  to += lt::ODO_MIN_INTERVAL_MS;
  o.update(in, to);
  o.status(&st);
  CHECK(st.writes == w0 + 2 && st.lastKm == 2 && st.today == nov8 + 1);
  CHECK(o.periodAt(OdoLog::LV_DAY, nov8, &pd) && pd.km == 20 && pd.odoEnd == 23520 &&
        o.periodAt(OdoLog::LV_WEEK, nov8 + 1, &pd) && pd.km == 22 && pd.odoEnd == 23522);

  // a half-written record (power lost): the next sector is used
  o.begin(&fl, g_table.data());
  o.update(in, to += 100);
  o.status(&st);
  const uint32_t r0 = st.records;
  const uint32_t nextOff = st.records * lt::ODO_REC_BYTES;  // all so far in the first sector
  fl.m[nextOff] = 0x12;
  o.begin(&fl, g_table.data());
  in.odoKm = 23523;
  to += lt::ODO_MIN_INTERVAL_MS;
  o.update(in, to);
  o.status(&st);
  CHECK(st.writes == 1 && st.failures == 0 && st.records == r0 + 1);
  CHECK(fl.m[lt::ODO_SECTOR_BYTES] == 0x44 && fl.m[lt::ODO_SECTOR_BYTES + 1] == 0x4F);
  o.begin(&fl, g_table.data());
  o.status(&st);
  CHECK(st.records == r0 + 1 && st.lastOdo == 23523);
  // write fails
  fl.writeOk = false;
  in.odoKm = 23524;
  to += lt::ODO_MIN_INTERVAL_MS;
  o.update(in, to);
  o.status(&st);
  CHECK(st.failures == 1 && st.writes == 0);
  fl.writeOk = true;

  // round the ring: more than 28,672 records, it goes on from the newest
  fl.clear();
  o.begin(&fl, g_table.data());
  in.speedKmh = 0;
  in.reverse = true;
  uint32_t odoKm = 30000;
  for (int i = 0; i < 28672 + 300; i++) {
    in.odoKm = ++odoKm;
    in.utc += 61;
    to += lt::ODO_MIN_INTERVAL_MS + 1000;
    o.update(in, to);
  }
  o.status(&st);
  CHECKF(st.writes >= 28672 + 290 && st.failures == 0 && st.records <= 28672 && st.records > 28672 - 128,
         "writes %u records %u failures %u", st.writes, st.records, st.failures);
  o.begin(&fl, g_table.data());
  o.status(&st);
  CHECK(st.lastOdo == odoKm && st.records > 28672 - 128);
  in.odoKm = ++odoKm;
  to += lt::ODO_MIN_INTERVAL_MS + 1000;
  o.update(in, to);
  o.begin(&fl, g_table.data());
  o.status(&st);
  CHECK(st.lastOdo == odoKm);
}

TEST(odolog_periods) {
  // records of 9/10 (22000-22050), 10/20 (-22340), 11/3 (-22520), today 11/8 (-22615)
  static RamFlash fl;
  static OdoLog o;
  o.begin(&fl, g_table.data());
  OdoLog::Input in;
  std::memset(&in, 0, sizeof(in));
  in.odoOk = in.odoHave = true;
  in.speedOk = true;
  in.timeOk = true;
  uint32_t to = 1000;
  struct DayRun {
    int m, d;
    uint32_t from, to;
  };
  static const DayRun kRuns[] = {{9, 10, 22000, 22050}, {10, 20, 22050, 22340}, {11, 3, 22340, 22520}, {11, 8, 22520, 22615}};
  for (const DayRun& r : kRuns) {
    in.utc = lt::gtime::epoch(2026, static_cast<uint8_t>(r.m), static_cast<uint8_t>(r.d), 1, 0, 0);
    in.odoKm = r.from;
    o.update(in, to += lt::ODO_MIN_INTERVAL_MS + 1000);
    in.odoKm = r.to;
    in.utc += 3600;
    o.update(in, to += lt::ODO_MIN_INTERVAL_MS + 1000);
  }
  const int32_t nov8 = OdoLog::daysFrom(2026, 11, 8), sep10 = OdoLog::daysFrom(2026, 9, 10);
  OdoLog::Period pd;
  CHECK(o.periodAt(OdoLog::LV_DAY, nov8 - 3, &pd) && pd.odoKnown && pd.odoEnd == 22520 && pd.km == 0 && !pd.hasRecord);
  CHECK(o.periodAt(OdoLog::LV_WEEK, nov8 - 1, &pd) && pd.odoStart == 22340 && pd.odoEnd == 22520 && pd.km == 180);
  CHECK(o.periodAt(OdoLog::LV_MONTH, OdoLog::daysFrom(2026, 10, 5), &pd) && pd.odoStart == 22050 && pd.odoEnd == 22340 &&
        pd.km == 290);
  CHECK(o.periodAt(OdoLog::LV_QUARTER, nov8, &pd) && pd.odoStart == 22050 && pd.odoEnd == 22615 && pd.km == 565 &&
        pd.canBack && !pd.canFwd);
  CHECK(o.periodAt(OdoLog::LV_QUARTER, sep10, &pd) && pd.odoStart == 22000 && pd.odoEnd == 22050 && !pd.canBack &&
        pd.canFwd);
  CHECK(o.periodAt(OdoLog::LV_YEAR, nov8, &pd) && pd.odoStart == 22000 && pd.odoEnd == 22615 && pd.km == 615 &&
        !pd.canBack && !pd.canFwd);

  // the screen's navigation (round gauge pc_test: 走行記録の画面, same steps and results)
  lt::OdoNav nv;
  nv.open(o);
  CHECK(nv.level() == OdoLog::LV_YEAR && nv.anchor() == nov8);
  CHECK(!nv.prev(o) && nv.anchor() == nov8);  // records only this year
  CHECK(nv.setLevel(OdoLog::LV_QUARTER, o) && nv.anchor() == nov8);
  CHECK(nv.setLevel(OdoLog::LV_MONTH, o) && nv.anchor() == nov8);
  CHECK(!nv.next(o) && nv.anchor() == nov8);  // not past the current period
  CHECK(nv.prev(o) && nv.anchor() == OdoLog::daysFrom(2026, 10, 31));
  CHECK(nv.setLevel(OdoLog::LV_WEEK, o) && nv.anchor() == OdoLog::daysFrom(2026, 10, 31));  // week 10/25-10/31
  CHECK(nv.setLevel(OdoLog::LV_DAY, o) && nv.anchor() == OdoLog::daysFrom(2026, 10, 31));
  CHECK(!nv.canFiner(o) && !nv.setLevel(OdoLog::LV_COUNT, o));
  CHECK(nv.prev(o) && nv.anchor() == OdoLog::daysFrom(2026, 10, 30));  // day by day, days without records too
  CHECK(nv.setLevel(OdoLog::LV_WEEK, o) && nv.setLevel(OdoLog::LV_MONTH, o) && nv.setLevel(OdoLog::LV_QUARTER, o));
  CHECK(o.periodAt(nv.level(), nv.anchor(), &pd) && pd.first == OdoLog::daysFrom(2026, 10, 1));
  CHECK(nv.prev(o) && nv.anchor() == OdoLog::daysFrom(2026, 9, 30));  // Jul-Sep
  CHECK(!nv.prev(o) && nv.anchor() == OdoLog::daysFrom(2026, 9, 30));  // not before the oldest record's period
  CHECK(nv.setLevel(OdoLog::LV_MONTH, o) && nv.setLevel(OdoLog::LV_WEEK, o));
  CHECK(o.periodAt(nv.level(), nv.anchor(), &pd) && pd.first == OdoLog::daysFrom(2026, 9, 27));  // 9/27-10/3
  CHECK(nv.setLevel(OdoLog::LV_MONTH, o) && nv.anchor() == OdoLog::daysFrom(2026, 9, 30));  // back to September
  CHECK(nv.setLevel(OdoLog::LV_YEAR, o) && nv.level() == OdoLog::LV_YEAR);
  {  // before the time is known nothing moves
    static OdoLog blank;
    RamFlash none;
    blank.begin(&none, nullptr);
    lt::OdoNav n2;
    n2.open(blank);
    CHECK(!n2.setLevel(OdoLog::LV_MONTH, blank) && !n2.prev(blank) && !n2.next(blank) && !n2.canFiner(blank));
  }

  // the round gauge's own bytes read back the same (format compatibility): one record encoded by hand
  RamFlash one;
  uint8_t r[32];
  std::memset(r, 0xFF, sizeof r);
  const uint8_t head[] = {0x44, 0x4F, 0x01, 0x02, 0x07, 0, 0, 0};
  std::memcpy(r, head, sizeof head);
  const uint32_t utc = lt::gtime::epoch(2026, 11, 8, 3, 0, 0);
  for (int i = 0; i < 4; i++) r[8 + i] = static_cast<uint8_t>(utc >> (8 * i));
  r[12] = static_cast<uint8_t>(nov8);
  r[13] = static_cast<uint8_t>(nov8 >> 8);
  const uint32_t vals[3] = {22615, 22520, 42};
  for (int k = 0; k < 3; k++)
    for (int i = 0; i < 4; i++) r[14 + 4 * k + i] = static_cast<uint8_t>(vals[k] >> (8 * i));
  uint16_t c = 0xFFFF;
  for (int i = 0; i < 30; i++) {
    c = static_cast<uint16_t>(c ^ (r[i] << 8));
    for (int b = 0; b < 8; b++) c = (c & 0x8000) ? static_cast<uint16_t>((c << 1) ^ 0x1021) : static_cast<uint16_t>(c << 1);
  }
  r[30] = static_cast<uint8_t>(c);
  r[31] = static_cast<uint8_t>(c >> 8);
  one.write(0, r, sizeof r);
  static OdoLog o2;
  o2.begin(&one, g_table.data());
  OdoLog::Status st;
  o2.status(&st);
  uint32_t km = 0, ab = 0;
  CHECKF(st.records == 1 && st.lastOdo == 22615 && st.lastKm == 95 && st.lastAboveS == 42 &&
             st.lastReason == OdoLog::R_LOW && o2.day(nov8, &km, &ab) && km == 95 && ab == 42,
         "records %u odo %u km %u", st.records, st.lastOdo, st.lastKm);
}
