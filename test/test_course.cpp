// core/course: courses.json, nearest course within 3 km, CourseDef, RaceChrono [laptiming] conversion.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/course.h"
#include "core/geo.h"
#include "core/json.h"
#include "core/lap.h"
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

// The [session data] and [laptiming] lines of the owner's RaceChrono Pro v10.3.1 VBO export
// (testdata/session_20261031_135027_*.vbo, CRLF, "¬" = U+00AC). The line after [data] must be ignored.
const char kVboExcerpt[] =
    "File created on 31/10/2026 at 04:50:23\r\n"
    "\r\n"
    "[header]\r\nsatellites\r\ntime\r\nlatitude\r\nlongitude\r\n\r\n"
    "[session data]\r\n"
    "name 袖ヶ浦フォレストレースウェイ\r\n"
    "\r\n"
    "[laptiming]\r\n"
    "Start   -8405.283480 +2123.694540 -8405.257911 +2123.659851 ¬ Start/Finish\r\n"
    "Split   -8405.457760 +2123.535530 -8405.488860 +2123.544758 ¬ Split 1\r\n"
    "Split   -8405.322360 +2123.640540 -8405.348440 +2123.657150 ¬ Split 2\r\n"
    "Split   -8405.352900 +2123.546460 -8405.384376 +2123.554797 ¬ Split 3\r\n"
    "\r\n"
    "[column names]\r\nsats time lat long\r\n\r\n"
    "[data]\r\n"
    "Split   -8405.000000 +2123.000000 -8405.100000 +2123.100000 ¬ not a line\r\n";

// 実装計画 §0: RaceChrono's definition of 袖ヶ浦
struct Expect {
  const char* name;
  int32_t latE7, lonE7;
  float heading, width;
};
const Expect kSodegaura[4] = {{"Start/Finish", 353949090, 1400880580, 31.1f, 50.0f},
                              {"Split 1", 353922588, 1400909627, 250.1f, 30.0f},
                              {"Split 2", 353940090, 1400887060, 232.1f, 30.0f},
                              {"Split 3", 353924410, 1400892150, 252.1f, 30.0f}};

float angleDiff(float a, float b) {
  float d = std::fabs(a - b);
  return d > 180.0f ? 360.0f - d : d;
}

void checkSodegaura(const lt::Course& c, const char* what, float tolDeg) {
  CHECKF(std::strcmp(c.name, "袖ヶ浦フォレストレースウェイ") == 0, "%s: name %s", what, c.name);
  CHECKF(c.numGates == 4, "%s: %d gates", what, c.numGates);
  CHECKF(c.minLapS == 30.0f, "%s: min lap %.1f", what, static_cast<double>(c.minLapS));
  for (int i = 0; i < 4 && i < c.numGates; ++i) {
    const lt::CourseGate& g = c.gates[i];
    const Expect& e = kSodegaura[i];
    CHECKF(g.type == (i == 0 ? lt::GateType::Sf : lt::GateType::Split), "%s gate %d: type", what, i);
    CHECKF(std::strcmp(g.name, e.name) == 0, "%s gate %d: name %s", what, i, g.name);
    CHECKF(g.latE7 == e.latE7 && g.lonE7 == e.lonE7, "%s gate %d: %d %d", what, i, g.latE7, g.lonE7);
    CHECKF(angleDiff(g.headingDeg, e.heading) <= tolDeg, "%s gate %d: heading %.3f (expected %.1f)", what, i,
           static_cast<double>(g.headingDeg), static_cast<double>(e.heading));
    CHECKF(g.widthM == e.width, "%s gate %d: width %.0f", what, i, static_cast<double>(g.widthM));
  }
}

const char* loadWhy(const char* text, lt::json::Error* e) {
  lt::CourseDb db;
  db.load(text, -1, e);
  return e->why;
}

}  // namespace

TEST(course_racechrono_vbo_to_course_sodegaura) {
  lt::Course c{};
  const char* why = "?";
  CHECKF(lt::CourseDb::parseRaceChronoVbo(kVboExcerpt, -1, "sodegaura", &c, &why), "why %s", why);
  CHECK(std::strcmp(c.id, "sodegaura") == 0);
  checkSodegaura(c, "parsed", 0.05f);
  for (int i = 0; i < c.numGates; ++i) {
    tmini::info("%-12s %.7f %.7f heading %.3f width %.0f", c.gates[i].name, c.gates[i].latE7 * 1e-7,
                c.gates[i].lonE7 * 1e-7, static_cast<double>(c.gates[i].headingDeg),
                static_cast<double>(c.gates[i].widthM));
  }
  // as courses.json text, loaded back
  char text[2048];
  const int n = lt::CourseDb::fromRaceChronoVbo(kVboExcerpt, -1, "sodegaura", text, sizeof text, &why);
  CHECKF(n > 0, "why %s", why);
  lt::json::MemSource src(text, n);
  lt::json::Error e{};
  CHECK(lt::json::check(src, &e));
  lt::CourseDb db;
  CHECKF(db.load(text, n, &e), "%s at %d:%d\n%s", e.why, e.line, e.col, text);
  CHECK(db.count() == 1);
  if (db.count() == 1) checkSodegaura(db.course(0), "json", 0.05f);
  // the text is stable: writing the loaded course gives the same text
  char again[2048];
  CHECK(db.toJson(again, sizeof again) == n && std::strcmp(again, text) == 0);
  // a buffer that is too small
  CHECK(lt::CourseDb::fromRaceChronoVbo(kVboExcerpt, -1, "sodegaura", text, 100, &why) == 0);
}

TEST(course_racechrono_vbo_errors_and_ids) {
  lt::Course c{};
  const char* why = "";
  CHECK(!lt::CourseDb::parseRaceChronoVbo("[header]\r\nsatellites\r\n", -1, nullptr, &c, &why));
  CHECK(std::strcmp(why, "no-laptiming") == 0);
  CHECK(!lt::CourseDb::parseRaceChronoVbo(
      "[laptiming]\nSplit -8405.457760 +2123.535530 -8405.488860 +2123.544758 ¬ Split 1\n", -1, nullptr, &c, &why));
  CHECK(std::strcmp(why, "no-start-line") == 0);
  CHECK(!lt::CourseDb::parseRaceChronoVbo(
      "[laptiming]\nStart -8405.283480 +2123.694540 -8405.257911 +2123.659851 ¬ A\n"
      "Start -8405.283480 +2123.694540 -8405.257911 +2123.659851 ¬ B\n",
      -1, nullptr, &c, &why));
  CHECK(std::strcmp(why, "two-start-lines") == 0);
  CHECK(!lt::CourseDb::parseRaceChronoVbo("[laptiming]\nStart -8405.28x +2123.69 -8405.25 +2123.65 ¬ A\n", -1,
                                          nullptr, &c, &why));
  CHECK(std::strcmp(why, "bad-laptiming-line") == 0);
  CHECK(!lt::CourseDb::parseRaceChronoVbo(kVboExcerpt, -1, "Sode Gaura", &c, &why));
  CHECK(std::strcmp(why, "bad-id") == 0);
  // no id given: from the ASCII letters of the name, else from the S/F position
  CHECK(lt::CourseDb::parseRaceChronoVbo(kVboExcerpt, -1, nullptr, &c, &why));
  CHECKF(std::strcmp(c.id, "course_n35394_e140088") == 0, "id %s", c.id);
  const char* tsukuba =
      "[session data]\nname Tsukuba Circuit 2000\n[laptiming]\n"
      "Start -8405.283480 +2123.694540 -8405.257911 +2123.659851 \xC2\xAC Start/Finish\n";
  CHECK(lt::CourseDb::parseRaceChronoVbo(tsukuba, -1, "", &c, &why));
  CHECKF(std::strcmp(c.id, "tsukuba_circuit_2000") == 0 && std::strcmp(c.name, "Tsukuba Circuit 2000") == 0,
         "id %s name %s", c.id, c.name);
  CHECK(c.numGates == 1 && c.gates[0].widthM == 50.0f);
}

TEST(course_data_file_sodegaura) {
  const std::string text = readFile("data/courses.json");
  CHECKF(!text.empty(), "data/courses.json not found (run from the repository root)");
  lt::json::MemSource src(text.data(), static_cast<int>(text.size()));
  lt::json::Error e{};
  CHECKF(lt::json::check(src, &e), "json::check: %s at %d:%d", e.why, e.line, e.col);
  lt::CourseDb db;
  CHECKF(db.load(text.data(), static_cast<int>(text.size()), &e), "load: %s at %d:%d", e.why, e.line, e.col);
  CHECK(db.count() == 1);
  const int i = db.find("sodegaura");
  CHECK(i == 0);
  if (i != 0) return;
  checkSodegaura(db.course(i), "data/courses.json", 0.0001f);  // the rounded values of §0
  // the same lines as converted from RaceChrono's export (headings within 0.05 deg)
  lt::Course c{};
  CHECK(lt::CourseDb::parseRaceChronoVbo(kVboExcerpt, -1, "sodegaura", &c, nullptr));
  for (int k = 0; k < 4; ++k) {
    CHECK(c.gates[k].latE7 == db.course(i).gates[k].latE7 && c.gates[k].lonE7 == db.course(i).gates[k].lonE7);
    CHECK(angleDiff(c.gates[k].headingDeg, db.course(i).gates[k].headingDeg) <= 0.05f);
  }
  lt::CourseDef def{};
  CHECK(db.buildCourseDef(i, &def));
  CHECK(def.numSectors == 3 && def.minLapS == 30.0f);
  CHECK(def.startFinish.latE7 == 353949090 && def.startFinish.lonE7 == 1400880580 && def.startFinish.widthM == 50.0f);
  CHECK(def.sectors[0].latE7 == 353922588 && def.sectors[2].lonE7 == 1400892150 && def.sectors[1].widthM == 30.0f);
  lt::json::MemSource src2(text.data(), static_cast<int>(text.size()));
  CHECK(lt::CourseDb::check(src2, &e));
}

TEST(course_nearest_within_3km) {
  const char* text =
      "{\"version\":1,\"courses\":["
      "{\"id\":\"sodegaura\",\"name\":\"袖ヶ浦\",\"gates\":[{\"type\":\"sf\",\"lat\":35.3949090,\"lon\":140.0880580,"
      "\"heading\":31.1},{\"type\":\"split\",\"lat\":35.3922588,\"lon\":140.0909627,\"heading\":250.1}]},"
      "{\"id\":\"far\",\"center\":[35.5,140.2],\"gates\":[{\"type\":\"sf\",\"lat\":35.4,\"lon\":140.09,\"heading\":0}]}]}";
  lt::CourseDb db;
  lt::json::Error e{};
  CHECKF(db.load(text, -1, &e), "%s at %d:%d", e.why, e.line, e.col);
  CHECK(db.count() == 2);
  const lt::Course& s = db.course(0);
  CHECK(!s.centerGiven && s.centerLatE7 == (353949090ll + 353922588ll) / 2 &&
        s.centerLonE7 == (1400880580ll + 1400909627ll) / 2);
  CHECK(db.course(1).centerGiven && db.course(1).centerLatE7 == 355000000);  // "center" wins over the gates
  lt::Plane pl;
  db.initPlane(0, &pl);
  struct P {
    float x, y;
    int expect;
  };
  const P pts[] = {{0, 0, 0}, {2900, 0, 0}, {0, -2900, 0}, {-2050, 2050, 0}, {3100, 0, -1}, {0, 3100, -1},
                   {2250, -2250, -1}};
  for (const P& p : pts) {
    int32_t lat = 0, lon = 0;
    pl.toGlobal({p.x, p.y}, &lat, &lon);
    float d = 0.0f;
    const int got = db.nearest(lat, lon, lt::kNearCourseM, &d);
    const float truth = std::sqrt(p.x * p.x + p.y * p.y);
    CHECKF(got == p.expect, "(%.0f, %.0f): nearest %d, expected %d", static_cast<double>(p.x),
           static_cast<double>(p.y), got, p.expect);
    const float dd = db.distanceM(0, lat, lon);
    CHECKF(std::fabs(dd - truth) <= 0.005f * truth + 0.5f, "distance %.1f m, plane %.1f m", static_cast<double>(dd),
           static_cast<double>(truth));
  }
  // the nearer of two courses; the far one alone at its centre
  CHECK(db.nearest(355000000, 1402000000) == 1);
  CHECK(db.nearest(355000000, 1402000000, 1e9f) == 1);
  CHECK(db.nearest(-335000000, -700000000) == -1);  // Chile
  CHECK(db.nearest(353949090, 1400880580, 1e9f) == 0);
}

TEST(course_load_errors) {
  struct Bad {
    const char* text;
    const char* why;
  };
  const std::string many = [] {
    std::string t = "{\"version\":1,\"courses\":[";
    for (int i = 0; i < 17; ++i) {
      t += std::string(i ? "," : "") + "{\"id\":\"c" + std::to_string(i) +
           "\",\"gates\":[{\"type\":\"sf\",\"lat\":35,\"lon\":140,\"heading\":0}]}";
    }
    return t + "]}";
  }();
  const std::string nineGates = [] {
    std::string t = "{\"version\":1,\"courses\":[{\"id\":\"a\",\"gates\":[{\"type\":\"sf\",\"lat\":35,\"lon\":140,\"heading\":0}";
    for (int i = 0; i < 8; ++i) t += ",{\"type\":\"split\",\"lat\":35,\"lon\":140,\"heading\":0}";
    return t + "]}]}";
  }();
  const std::string longName = "{\"version\":1,\"courses\":[{\"id\":\"a\",\"name\":\"" + std::string(96, 'x') +
                               "\",\"gates\":[{\"type\":\"sf\",\"lat\":35,\"lon\":140,\"heading\":0}]}]}";
#define G "\"gates\":[{\"type\":\"sf\",\"lat\":35,\"lon\":140,\"heading\":0}]"
  const Bad bad[] = {
      {"{\"courses\":[]}", "missing-version"},
      {"{\"version\":2,\"courses\":[]}", "bad-version"},
      {"{\"version\":1}", "missing-courses"},
      {"{\"version\":1,\"courses\":[{" G "}]}", "missing-id"},
      {"{\"version\":1,\"courses\":[{\"id\":\"Sode\"," G "}]}", "bad-id"},
      {"{\"version\":1,\"courses\":[{\"id\":\"a\"," G "},{\"id\":\"a\"," G "}]}", "duplicate-id"},
      {"{\"version\":1,\"courses\":[{\"id\":\"a\",\"min_lap_s\":1," G "}]}", "bad-min-lap"},
      {"{\"version\":1,\"courses\":[{\"id\":\"a\",\"center\":[35]," G "}]}", "bad-center"},
      {"{\"version\":1,\"courses\":[{\"id\":\"a\",\"center\":[35,140,1]," G "}]}", "bad-center"},
      {"{\"version\":1,\"courses\":[{\"id\":\"a\",\"gates\":[]}]}", "no-sf"},
      {"{\"version\":1,\"courses\":[{\"id\":\"a\",\"gates\":[{\"type\":\"sf\",\"lat\":35,\"lon\":140,\"heading\":0},"
       "{\"type\":\"sf\",\"lat\":35,\"lon\":140,\"heading\":0}]}]}",
       "two-sf"},
      {"{\"version\":1,\"courses\":[{\"id\":\"a\",\"gates\":[{\"type\":\"finish\",\"lat\":35,\"lon\":140,\"heading\":0}]}]}",
       "bad-gate-type"},
      {"{\"version\":1,\"courses\":[{\"id\":\"a\",\"gates\":[{\"type\":\"sf\",\"lat\":95,\"lon\":140,\"heading\":0}]}]}",
       "bad-lat"},
      {"{\"version\":1,\"courses\":[{\"id\":\"a\",\"gates\":[{\"type\":\"sf\",\"lat\":35,\"lon\":181,\"heading\":0}]}]}",
       "bad-lon"},
      {"{\"version\":1,\"courses\":[{\"id\":\"a\",\"gates\":[{\"type\":\"sf\",\"lat\":35,\"lon\":140,\"heading\":-1}]}]}",
       "bad-heading"},
      {"{\"version\":1,\"courses\":[{\"id\":\"a\",\"gates\":[{\"type\":\"sf\",\"lat\":35,\"lon\":140,\"heading\":0,"
       "\"width\":0}]}]}",
       "bad-width"},
      {"{\"version\":1,\"courses\":[{\"id\":\"a\",\"gates\":[{\"type\":\"sf\",\"lat\":35,\"lon\":140}]}]}",
       "missing-heading"},
      {"{\"version\":1,\"courses\":[{\"id\":\"a\",\"gates\":[{\"lat\":35,\"lon\":140,\"heading\":0}]}]}", "missing-type"},
      {"{\"version\":1,\"courses\":[{\"id\":\"a\",\"gates\":[{\"type\":\"sf\",\"lat\":\"35\",\"lon\":140,\"heading\":0}]}]}",
       "expected-number"},
      {many.c_str(), "too-many-courses"},
      {nineGates.c_str(), "too-many-gates"},
      {longName.c_str(), "name-too-long"},
      {"{\"version\":1,\"courses\":[{\"id\":\"a\" " G "}]}", "expected-comma-or-brace"},
  };
#undef G
  for (const Bad& b : bad) {
    lt::json::Error e{};
    const char* why = loadWhy(b.text, &e);
    CHECKF(std::strcmp(why, b.why) == 0, "expected %s, got %s at %d:%d in %.60s", b.why, why, e.line, e.col, b.text);
    lt::json::MemSource src(b.text);
    lt::json::Error e2{};
    CHECK(!lt::CourseDb::check(src, &e2) && std::strcmp(e2.why, b.why) == 0 && e2.line == e.line && e2.col == e.col);
  }
  // a failed load leaves the database empty
  lt::CourseDb db;
  lt::json::Error e{};
  CHECK(db.load("{\"version\":1,\"courses\":[{\"id\":\"a\",\"gates\":[{\"type\":\"sf\",\"lat\":35,\"lon\":140,"
                "\"heading\":0}]}]}",
                -1, &e));
  CHECK(db.count() == 1);
  CHECK(!db.load("{\"version\":1,\"courses\":[{\"id\":\"a\",\"gates\":[]}]}", -1, &e));
  CHECK(db.count() == 0);
}

TEST(course_defaults_put_remove_round_trip) {
  lt::CourseDb db;
  lt::json::Error e{};
  CHECK(db.load("{\"_note\":\"x\",\"version\":1,\"courses\":[{\"id\":\"a\",\"extra\":[1,{\"b\":2}],\"gates\":["
                "{\"type\":\"sf\",\"lat\":35,\"lon\":140,\"heading\":360},{\"type\":\"split\",\"lat\":35.001,"
                "\"lon\":140,\"heading\":90}]}]}",
                -1, &e));
  CHECK(db.count() == 1);
  const lt::Course& a = db.course(0);
  CHECK(std::strcmp(a.name, "a") == 0 && a.minLapS == 30.0f);
  CHECK(a.gates[0].headingDeg == 0.0f && a.gates[0].widthM == 50.0f && a.gates[1].widthM == 30.0f);
  lt::Course b = a;
  std::strcpy(b.id, "b");
  std::strcpy(b.name, "Bコース");
  b.centerGiven = true;
  b.centerLatE7 = 351234567;
  b.centerLonE7 = -1;
  CHECK(db.put(b));
  CHECK(db.count() == 2);
  b.minLapS = 45.0f;
  CHECK(db.put(b));  // replaces "b"
  CHECK(db.count() == 2 && db.course(1).minLapS == 45.0f);
  lt::Course bad = b;
  std::strcpy(bad.id, "Bad");
  CHECK(!db.put(bad));
  char text[4096];
  const int n = db.toJson(text, sizeof text);
  CHECK(n > 0);
  lt::CourseDb db2;
  CHECKF(db2.load(text, n, &e), "%s at %d:%d\n%s", e.why, e.line, e.col, text);
  CHECK(db2.count() == 2);
  for (int k = 0; k < 2; ++k) {
    const lt::CourseGate& g1 = db.course(1).gates[k];
    const lt::CourseGate& g2 = db2.course(1).gates[k];
    CHECK(g1.type == g2.type && std::strcmp(g1.name, g2.name) == 0 && g1.latE7 == g2.latE7 &&
          g1.lonE7 == g2.lonE7 && g1.headingDeg == g2.headingDeg && g1.widthM == g2.widthM);
  }
  CHECK(db2.course(1).centerGiven && db2.course(1).centerLonE7 == -1 && db2.course(1).minLapS == 45.0f);
  CHECK(std::strcmp(db2.course(1).name, "Bコース") == 0);
  CHECK(db2.remove("a") && db2.count() == 1 && db2.find("b") == 0 && db2.find("a") == -1);
  CHECK(!db2.remove("a"));
  CHECK(lt::CourseDb::validId("sodegaura_2") && !lt::CourseDb::validId("") &&
        !lt::CourseDb::validId("a-b") && !lt::CourseDb::validId(std::string(32, 'a').c_str()) &&
        lt::CourseDb::validId(std::string(31, 'a').c_str()));
}

TEST(course_lap_timer_through_converted_lines) {
  // a car driving straight through the converted S/F line along its direction: one start, then
  // the lap after 60 s; the reverse pass does not count
  lt::Course c{};
  CHECK(lt::CourseDb::parseRaceChronoVbo(kVboExcerpt, -1, "sodegaura", &c, nullptr));
  lt::CourseDb db;
  CHECK(db.put(c));
  lt::CourseDef def{};
  CHECK(db.buildCourseDef(0, &def));
  def.numSectors = 0;
  lt::Plane pl;
  db.initPlane(0, &pl);
  lt::LapTimer t;
  t.begin(pl, def);
  const lt::Vec2 sf = pl.toLocal(def.startFinish.latE7, def.startFinish.lonE7);
  const lt::Vec2 h = lt::headingUnit(def.startFinish.headingDeg);
  int starts = 0, laps = 0;
  int64_t lapUs = 0;
  for (int pass = 0; pass < 3; ++pass) {
    const float dir = pass == 1 ? -1.0f : 1.0f;  // pass 1 drives the other way
    for (int k = 0; k <= 100; ++k) {
      const float s = (static_cast<float>(k) - 50.0f) * 1.0f * dir;  // 25 m/s, 25 Hz
      lt::Fix f{};
      pl.toGlobal(sf + h * s, &f.latE7, &f.lonE7);
      f.tUs = static_cast<int64_t>(pass) * 30000000 + static_cast<int64_t>(k) * 40000;
      f.speedMps = 25.0f;
      f.valid = true;
      const uint32_t ev = t.feed(f);
      if (ev & lt::kEvLapStart) ++starts;
      if (ev & lt::kEvLap) {
        ++laps;
        lapUs = t.lastLap().lapUs;
      }
    }
  }
  CHECK(starts == 1 && laps == 1);
  CHECKF(std::llabs(lapUs - 60000000) <= 1000, "lap %lld us", static_cast<long long>(lapUs));
}
