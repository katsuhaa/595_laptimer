// core/maint: dates, maint.json, remaining km / days / track hours, states, order, mark done.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/json.h"
#include "core/maint.h"
#include "tmini.h"

namespace {

std::string readFile(const char* path) {
  std::string s;
  FILE* f = std::fopen(path, "rb");
  if (!f) return s;
  char b[4096];
  size_t n = 0;
  while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
  std::fclose(f);
  return s;
}

int32_t day(const char* s) {
  int32_t d = lt::kNoDate;
  lt::parseDate(s, static_cast<int>(std::strlen(s)), &d);
  return d;
}

lt::MaintNow now(const char* date, float odo, float trackH) {
  lt::MaintNow n;
  n.today = date ? day(date) : lt::kNoDate;
  n.odoOk = odo >= 0.0f;
  n.odoKm = odo;
  n.trackOk = trackH >= 0.0f;
  n.trackH = trackH;
  return n;
}

const char kArch[] =
    "{\"version\":1,\"items\":[{\"id\":\"oil\",\"name\":\"エンジンオイル\",\"interval_km\":5000,\"interval_days\":182,"
    "\"interval_track_h\":0,\"last_date\":\"2026-10-01\",\"last_odo\":12345,\"last_track_h\":12.5,\"note\":\"\"}]}";

bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

}  // namespace

TEST(maint_dates) {
  CHECK(lt::daysFromCivil(1970, 1, 1) == 0);
  CHECK(lt::daysFromCivil(2000, 3, 1) == 11017);
  CHECK(lt::daysFromCivil(1969, 12, 31) == -1);
  int bad = 0;
  for (int32_t d = -800000; d < 3000000; d += 37) {
    int y = 0, m = 0, dd = 0;
    lt::civilFromDays(d, &y, &m, &dd);
    if (lt::daysFromCivil(y, m, dd) != d || m < 1 || m > 12 || dd < 1 || dd > 31) ++bad;
  }
  CHECKF(bad == 0, "%d day numbers do not survive", bad);
  int32_t d = 0;
  CHECK(lt::parseDate("2028-02-29", 10, &d) && d == lt::daysFromCivil(2028, 2, 29));
  CHECK(!lt::parseDate("2026-02-29", 10, &d));
  CHECK(!lt::parseDate("2026-13-01", 10, &d));
  CHECK(!lt::parseDate("2026-1-01", 9, &d));
  CHECK(!lt::parseDate("2026/01/01", 10, &d));
  CHECK(!lt::parseDate("1899-12-31", 10, &d));
  char t[16];
  CHECK(lt::formatDate(t, sizeof t, lt::daysFromCivil(2026, 11, 8)) == 10 && std::strcmp(t, "2026-11-08") == 0);
  CHECK(lt::formatDate(t, 10, 0) == 0);
  CHECK(lt::formatDate(t, sizeof t, lt::kNoDate) == 0);
  // JST: 2026-11-07 15:00 UTC is already 11-08 in Japan
  const int64_t utc = static_cast<int64_t>(lt::daysFromCivil(2026, 11, 7)) * 86400000 + 15ll * 3600000;
  CHECK(lt::dayOfUtcMs(utc, 540) == lt::daysFromCivil(2026, 11, 8));
  CHECK(lt::dayOfUtcMs(utc - 1, 540) == lt::daysFromCivil(2026, 11, 7));
  CHECK(lt::dayOfUtcMs(-1, 0) == -1);
}

TEST(maint_arch_example_remaining) {
  lt::Maint m;
  lt::json::Error e{};
  CHECKF(m.load(kArch, -1, &e), "%s at %d:%d", e.why, e.line, e.col);
  CHECK(m.count() == 1);
  const lt::MaintItem& it = m.item(0);
  CHECK(std::strcmp(it.id, "oil") == 0 && std::strcmp(it.name, "エンジンオイル") == 0);
  CHECK(it.intervalKm == 5000 && it.intervalDays == 182 && it.intervalTrackH == 0.0f);
  CHECK(it.lastDay == day("2026-10-01") && it.hasOdo && it.lastOdoKm == 12345.0f && it.hasTrackH);
  // 38 days and 655 km later: days are closer (144 of 182 left) than km (4345 of 5000)
  lt::MaintStatus s = m.status(0, now("2026-11-08", 13000.0f, 20.0f));
  CHECK(s.hasKm && s.hasDays && !s.hasTrackH);  // interval_track_h 0 = not used
  CHECK(near(s.remKm, 4345.0f, 0.01f) && near(s.remDays, 144.0f, 0.0f));
  CHECK(s.unit == lt::MaintUnit::Days && near(s.remaining, 144.0f, 0.0f) && near(s.fraction, 144.0f / 182.0f, 1e-6f));
  CHECK(s.state == lt::MaintState::Ok);
  // without the date only km is evaluated
  s = m.status(0, now(nullptr, 13000.0f, 20.0f));
  CHECK(s.hasKm && !s.hasDays && s.unit == lt::MaintUnit::Km && s.state == lt::MaintState::Ok);
  // nothing known now: unknown
  s = m.status(0, now(nullptr, -1.0f, -1.0f));
  CHECK(s.state == lt::MaintState::Unknown && s.unit == lt::MaintUnit::None);
}

TEST(maint_states_and_order) {
  // now: 2026-11-08, ODO 500 km, 12 track hours
  const char* text =
      "{\"version\":1,\"items\":["
      "{\"id\":\"ok_km\",\"interval_km\":1000,\"last_odo\":0},"                   // 500 of 1000 left: 50 %
      "{\"id\":\"ok80\",\"interval_km\":1000,\"last_odo\":300},"                  // 800 left: 80 %
      "{\"id\":\"due0\",\"interval_days\":10,\"last_date\":\"2026-10-29\"},"       // due today: 0 % = overdue
      "{\"id\":\"late\",\"interval_track_h\":10,\"last_track_h\":0},"            // 12 of 10 h used: -20 %
      "{\"id\":\"unknown\",\"interval_km\":1000},"                               // never recorded
      "{\"id\":\"none\"},"                                                       // no interval
      "{\"id\":\"warn10\",\"interval_days\":100,\"last_date\":\"2026-08-10\"},"  // 10 of 100 days: 10 %
      "{\"id\":\"ok_tie\",\"interval_km\":1000,\"last_odo\":0}"                   // 50 %, same as ok_km
      "]}";
  lt::Maint m;
  lt::json::Error e{};
  CHECKF(m.load(text, -1, &e), "%s at %d:%d", e.why, e.line, e.col);
  const lt::MaintNow n = now("2026-11-08", 500.0f, 12.0f);
  lt::MaintStatus all[lt::kMaintMaxItems];
  const int k = m.evaluate(n, all, lt::kMaintMaxItems);
  CHECK(k == 8);
  // most overdue first, then warn, then ok by fraction left (ties in file order), unknown last
  const char* expect[8] = {"late", "due0", "warn10", "ok_km", "ok_tie", "ok80", "unknown", "none"};
  for (int i = 0; i < 8 && i < k; ++i) {
    CHECKF(std::strcmp(m.item(all[i].index).id, expect[i]) == 0, "%d: %s, expected %s", i, m.item(all[i].index).id,
           expect[i]);
  }
  CHECK(all[0].state == lt::MaintState::Overdue && all[0].unit == lt::MaintUnit::TrackH &&
        near(all[0].remaining, -2.0f, 1e-5f) && near(all[0].fraction, -0.2f, 1e-6f));
  CHECK(all[1].state == lt::MaintState::Overdue && all[1].unit == lt::MaintUnit::Days && all[1].remaining == 0.0f);
  CHECK(all[2].state == lt::MaintState::Warn && near(all[2].fraction, 0.1f, 1e-6f) && all[2].remaining == 10.0f);
  CHECK(all[3].state == lt::MaintState::Ok && near(all[3].remaining, 500.0f, 0.0f) && all[5].state == lt::MaintState::Ok);
  CHECK(all[6].state == lt::MaintState::Unknown && all[7].state == lt::MaintState::Unknown);
  // the 20 % boundary is warn, 0 left is overdue
  lt::Maint b;
  CHECK(b.load("{\"version\":1,\"items\":[{\"id\":\"a\",\"interval_km\":1000,\"last_odo\":0}]}", -1, &e));
  CHECK(b.status(0, now(nullptr, 800.0f, -1.0f)).state == lt::MaintState::Warn);     // 200 left = 20 %
  CHECK(b.status(0, now(nullptr, 799.0f, -1.0f)).state == lt::MaintState::Ok);       // 201 left
  CHECK(b.status(0, now(nullptr, 1000.0f, -1.0f)).state == lt::MaintState::Overdue);  // 0 left
  // cap smaller than the list
  CHECK(m.evaluate(n, all, 3) == 3 && std::strcmp(m.item(all[0].index).id, "late") == 0);
}

TEST(maint_mark_done_and_write_back) {
  lt::Maint m;
  lt::json::Error e{};
  CHECK(m.load(kArch, -1, &e));
  CHECK(!m.markDone("oil", now(nullptr, 20000.0f, 30.0f)));  // date unknown: nothing changes
  CHECK(m.item(0).lastDay == day("2026-10-01"));
  CHECK(!m.markDone("nope", now("2026-11-08", 1.0f, 1.0f)));
  CHECK(m.markDone("oil", now("2026-11-08", 20000.4f, 30.25f)));
  const lt::MaintItem& it = m.item(0);
  CHECK(it.lastDay == day("2026-11-08") && it.hasOdo && near(it.lastOdoKm, 20000.4f, 0.01f) && it.lastTrackH == 30.25f);
  const lt::MaintStatus s = m.status(0, now("2026-11-08", 20000.4f, 30.25f));
  CHECK(s.state == lt::MaintState::Ok && s.fraction == 1.0f);
  char text[1024];
  const int n = m.toJson(text, sizeof text);
  const char* expect =
      "{\n  \"version\": 1,\n  \"items\": [\n"
      "    {\"id\": \"oil\", \"name\": \"エンジンオイル\", \"interval_km\": 5000, \"interval_days\": 182,"
      " \"interval_track_h\": 0, \"last_date\": \"2026-11-08\", \"last_odo\": 20000.4, \"last_track_h\": 30.25,"
      " \"note\": \"\"}\n  ]\n}\n";
  CHECKF(n == static_cast<int>(std::strlen(expect)) && std::strcmp(text, expect) == 0, "got:\n%s", text);
  // unknown ODO when marked: written as null, and read back as not recorded
  CHECK(m.markDone("oil", now("2026-11-09", -1.0f, 31.0f)));
  CHECK(m.toJson(text, sizeof text) > 0 && std::strstr(text, "\"last_odo\": null") != nullptr);
  lt::Maint m2;
  CHECKF(m2.load(text, -1, &e), "%s", e.why);
  CHECK(!m2.item(0).hasOdo && m2.item(0).lastDay == day("2026-11-09") && m2.item(0).lastTrackH == 31.0f);
  CHECK(m.toJson(text, 50) == 0);
}

TEST(maint_data_file_initial_items) {
  const std::string text = readFile("data/maint.json");
  CHECKF(!text.empty(), "data/maint.json not found (run from the repository root)");
  lt::json::MemSource src(text.data(), static_cast<int>(text.size()));
  lt::json::Error e{};
  CHECKF(lt::json::check(src, &e), "json::check: %s at %d:%d", e.why, e.line, e.col);
  lt::Maint m;
  CHECKF(m.load(text.data(), static_cast<int>(text.size()), &e), "load: %s at %d:%d", e.why, e.line, e.col);
  // UI設計 §9: oil 5000 km / 6 months, filter 10000 km, pads front/rear (track hours), fluid 12 months /
  // 10 track hours, coolant 24 months, plugs 20000 km
  struct Want {
    const char* id;
    int km, days;
    float h;
  };
  const Want want[] = {{"oil", 5000, 182, 0.0f},       {"oil_filter", 10000, 0, 0.0f}, {"brake_pad_f", 0, 0, 0.0f},
                       {"brake_pad_r", 0, 0, 0.0f},    {"brake_fluid", 0, 365, 10.0f}, {"coolant", 0, 730, 0.0f},
                       {"plug", 20000, 0, 0.0f}};
  CHECKF(m.count() == 7, "%d items", m.count());
  for (int i = 0; i < 7 && i < m.count(); ++i) {
    const lt::MaintItem& it = m.item(i);
    CHECKF(std::strcmp(it.id, want[i].id) == 0 && it.intervalKm == want[i].km && it.intervalDays == want[i].days &&
               it.intervalTrackH == want[i].h,
           "item %d: %s", i, it.id);
    CHECKF(std::strcmp(it.note, "案: 整備記録に合わせて直す") == 0, "item %d note: %s", i, it.note);
    CHECK(it.lastDay == lt::kNoDate && !it.hasOdo && !it.hasTrackH);  // nothing recorded yet
  }
  // nothing recorded: every item unknown, then the oil change is marked done
  const lt::MaintNow n = now("2026-11-08", 45000.0f, 3.0f);
  lt::MaintStatus all[lt::kMaintMaxItems];
  CHECK(m.evaluate(n, all, lt::kMaintMaxItems) == 7);
  for (int i = 0; i < 7; ++i) CHECK(all[i].state == lt::MaintState::Unknown);
  CHECK(m.markDone("oil", n));
  CHECK(m.evaluate(n, all, lt::kMaintMaxItems) == 7);
  CHECK(std::strcmp(m.item(all[0].index).id, "oil") == 0 && all[0].state == lt::MaintState::Ok);
  // due 2026-11-08 + 182 days = 2027-05-09: on 04-01 38 days are left (20.9 %, ok), on 04-03 36 (19.8 %, warn)
  lt::MaintStatus later = m.status(m.find("oil"), now("2027-04-01", 47000.0f, 3.0f));
  CHECK(later.unit == lt::MaintUnit::Days && later.state == lt::MaintState::Ok && later.remaining == 38.0f);
  later = m.status(m.find("oil"), now("2027-04-03", 47000.0f, 3.0f));
  CHECK(later.unit == lt::MaintUnit::Days && later.state == lt::MaintState::Warn && later.remaining == 36.0f);
  lt::json::MemSource src2(text.data(), static_cast<int>(text.size()));
  CHECK(lt::Maint::check(src2, &e));
}

TEST(maint_load_errors) {
  struct Bad {
    const char* text;
    const char* why;
  };
  const Bad bad[] = {
      {"{\"items\":[]}", "missing-version"},
      {"{\"version\":3,\"items\":[]}", "bad-version"},
      {"{\"version\":1}", "missing-items"},
      {"{\"version\":1,\"items\":[{\"name\":\"x\"}]}", "missing-id"},
      {"{\"version\":1,\"items\":[{\"id\":\"Oil\"}]}", "bad-id"},
      {"{\"version\":1,\"items\":[{\"id\":\"a\"},{\"id\":\"a\"}]}", "duplicate-id"},
      {"{\"version\":1,\"items\":[{\"id\":\"a\",\"interval_km\":-1}]}", "bad-interval"},
      {"{\"version\":1,\"items\":[{\"id\":\"a\",\"interval_km\":10.5}]}", "not-integer"},
      {"{\"version\":1,\"items\":[{\"id\":\"a\",\"interval_track_h\":-0.5}]}", "bad-interval"},
      {"{\"version\":1,\"items\":[{\"id\":\"a\",\"last_date\":\"2026-02-30\"}]}", "bad-date"},
      {"{\"version\":1,\"items\":[{\"id\":\"a\",\"last_date\":20261001}]}", "expected-string"},
      {"{\"version\":1,\"items\":[{\"id\":\"a\",\"last_odo\":-1}]}", "bad-odo"},
      {"{\"version\":1,\"items\":[{\"id\":\"a\",\"last_track_h\":-1}]}", "bad-track-h"},
      {"{\"version\":1,\"items\":[{\"id\":\"a\",\"name\":\"0123456789012345678901234567890123456789012345678901234567890123\"}]}",
       "name-too-long"},
      {"{\"version\":1,\"items\":[{\"id\":\"a\"} {\"id\":\"b\"}]}", "expected-comma-or-bracket"},
  };
  for (const Bad& b : bad) {
    lt::Maint m;
    lt::json::Error e{};
    CHECKF(!m.load(b.text, -1, &e) && std::strcmp(e.why, b.why) == 0, "expected %s, got %s in %.70s", b.why, e.why,
           b.text);
    CHECK(m.count() == 0);
  }
  std::string many = "{\"version\":1,\"items\":[";
  for (int i = 0; i < 17; ++i) many += std::string(i ? "," : "") + "{\"id\":\"i" + std::to_string(i) + "\"}";
  many += "]}";
  lt::Maint m;
  lt::json::Error e{};
  CHECK(!m.load(many.c_str(), -1, &e) && std::strcmp(e.why, "too-many-items") == 0);
  // null and "" mean "not recorded"; unknown keys are ignored
  CHECK(m.load("{\"version\":1,\"x\":{},\"items\":[{\"id\":\"a\",\"last_date\":null,\"last_odo\":null,\"y\":[1]}]}", -1, &e));
  CHECK(m.count() == 1 && m.item(0).lastDay == lt::kNoDate && !m.item(0).hasOdo && std::strcmp(m.item(0).name, "a") == 0);
}
