// core/replay: a synthetic session written with csvlog, read back and timed again gives the same
// fixes and laps to the microsecond; header-driven columns, empty and broken fields.
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "common.h"
#include "core/csvlog.h"
#include "core/lap.h"
#include "core/replay.h"
#include "synth.h"
#include "tmini.h"

namespace {

bool sameLap(const lt::LapResult& a, const lt::LapResult& b) {
  if (a.lapNo != b.lapNo || a.startUs != b.startUs || a.endUs != b.endUs || a.lapUs != b.lapUs ||
      a.numSplits != b.numSplits) {
    return false;
  }
  for (int i = 0; i < a.numSplits; ++i) {
    if (a.splitUs[i] != b.splitUs[i]) return false;
  }
  return true;
}

std::vector<std::string> lines(const std::string& text) {
  std::vector<std::string> v;
  size_t p = 0;
  while (p < text.size()) {
    const size_t e = text.find('\n', p);
    v.push_back(text.substr(p, (e == std::string::npos ? text.size() : e + 1) - p));
    p = e == std::string::npos ? text.size() : e + 1;
  }
  return v;
}

}  // namespace

TEST(replay_synthetic_session_same_laps) {
  const synth::Track tr;
  const lt::Plane pl = tcommon::plane();
  synth::RunConfig cfg;
  cfg.noiseM = 0.3;
  cfg.seed = 7;
  cfg.t0Us = 1794101400000000ll + 123;  // UTC [us]; GNSS times on the device are whole ms
  const synth::Run run = synth::makeRun(tr, pl, cfg);
  const lt::CourseDef course = tcommon::stadiumCourse(tr, pl, cfg.sfS, 2);

  // live: row -> fixOf -> lap timer -> lap fields -> CSV line (as the logger will do)
  lt::LapTimer live;
  live.begin(pl, course);
  std::string csv = lt::csvlog::kMainHeader;
  std::string lapsCsv = lt::csvlog::kLapsHeader;
  std::vector<lt::Fix> liveFixes;
  std::vector<lt::LapResult> liveLaps;
  char buf[512];
  for (size_t i = 0; i < run.fixes.size(); ++i) {
    const lt::Fix& g = run.fixes[i];
    lt::csvlog::MainRow r;
    r.utcOk = true;
    r.utcMs = (g.tUs + 500) / 1000;
    r.posOk = true;
    r.latE7 = g.latE7;
    r.lonE7 = g.lonE7;
    r.speedOk = true;
    r.speedKmh = g.speedMps * 3.6f;
    r.fixOk = true;
    r.fix = i % 211 == 100 ? 2 : 3;  // now and then a 2D fix: not used, live or replayed
    r.satsOk = true;
    r.sats = 18;
    r.rpmOk = i % 97 != 5;  // now and then a value is missing
    r.rpm = 3000 + static_cast<int>(i % 3000);
    r.altOk = i % 89 != 3;
    r.altM = 40.0f + static_cast<float>(i % 50) * 0.1f;
    r.gearOk = true;
    r.gear = static_cast<char>('1' + i % 5);
    const lt::Fix f = lt::csvlog::fixOf(r);
    const uint32_t ev = live.feed(f);
    if (ev & lt::kEvLap) {
      liveLaps.push_back(live.lastLap());
      lt::csvlog::lapsLine(buf, sizeof buf, live.lastLap());
      lapsCsv += buf;
    }
    lt::csvlog::setLapFields(&r, live, f.tUs);
    CHECK(lt::csvlog::mainLine(buf, sizeof buf, r) > 0);
    csv += buf;
    liveFixes.push_back(f);
  }
  CHECKF(liveLaps.size() == 5, "live laps %zu", liveLaps.size());

  // replay: parse every line, time it again, compare
  lt::Replay rp;
  const std::vector<std::string> ls = lines(csv);
  CHECK(rp.header(ls[0].c_str(), static_cast<int>(ls[0].size())));
  lt::LapTimer again;
  again.begin(pl, course);
  std::vector<lt::LapResult> laps;
  std::string lapsCsv2 = lt::csvlog::kLapsHeader;
  int fixDiff = 0, fieldDiff = 0, bad = 0;
  for (size_t i = 1; i < ls.size(); ++i) {
    lt::ReplayRow rr{};
    const char* why = "";
    if (!rp.parse(ls[i].c_str(), static_cast<int>(ls[i].size()), &rr, &why)) {
      ++bad;
      continue;
    }
    bad += rr.badFields;
    const lt::Fix& lf = liveFixes[i - 1];
    if (rr.fix.tUs != lf.tUs || rr.fix.latE7 != lf.latE7 || rr.fix.lonE7 != lf.lonE7 ||
        std::memcmp(&rr.fix.speedMps, &lf.speedMps, sizeof(float)) != 0 || rr.fix.valid != lf.valid) {
      ++fixDiff;
    }
    const uint32_t ev = again.feed(rr.fix);
    if (ev & lt::kEvLap) {
      laps.push_back(again.lastLap());
      lt::csvlog::lapsLine(buf, sizeof buf, again.lastLap());
      lapsCsv2 += buf;
    }
    lt::csvlog::MainRow mine;  // what the replayed timer says, against the values written live
    lt::csvlog::setLapFields(&mine, again, rr.fix.tUs);
    if (mine.lap != rr.row.lap || mine.sector != rr.row.sector || mine.lapMsOk != rr.row.lapMsOk ||
        (mine.lapMsOk && mine.lapMs != rr.row.lapMs)) {
      ++fieldDiff;
    }
  }
  CHECKF(bad == 0 && fixDiff == 0 && fieldDiff == 0, "bad %d, fixes differing %d, lap fields differing %d", bad,
         fixDiff, fieldDiff);
  CHECKF(laps.size() == liveLaps.size(), "replayed laps %zu", laps.size());
  for (size_t i = 0; i < laps.size() && i < liveLaps.size(); ++i) {
    CHECKF(sameLap(laps[i], liveLaps[i]), "lap %zu differs", i + 1);
  }
  CHECK(lapsCsv2 == lapsCsv);
  tmini::info("%zu rows, %zu laps: replayed fixes, laps (us) and lap/sector/lap_ms identical",
              ls.size() - 1, laps.size());
}

TEST(replay_header_driven_columns_and_broken_lines) {
  lt::Replay rp;
  const char* wrongOrder = "\xEF\xBB\xBFlap,utc_ms,note,lat,lon,speed_kmh,fix,rpm\r\n";
  CHECK(rp.header(wrongOrder));
  CHECK(rp.numColumns() == 8 && rp.column("rpm") == 7 && rp.column("utc_ms") == 1 && rp.column("alt_m") == -1);
  lt::ReplayRow r{};
  const char* why = "";
  const char* l1 = "3,1794101400040,hello,35.3949090,140.0880580,123.46,3,5500\r\n";
  CHECK(rp.parse(l1, -1, &r, &why));
  CHECK(r.row.lap == 3 && r.row.utcOk && r.row.utcMs == 1794101400040ll && r.row.posOk);
  CHECK(r.row.latE7 == 353949090 && r.row.lonE7 == 1400880580 && r.row.speedOk && r.row.rpmOk && r.row.rpm == 5500);
  CHECK(!r.row.altOk && !r.row.imuOk && !r.row.lapMsOk && r.badFields == 0);
  CHECK(r.fix.valid && r.fix.tUs == 1794101400040000ll && r.fix.speedMps == 12346.0f / 360.0f);
  const char* s = nullptr;
  int n = 0;
  CHECK(lt::Replay::field(l1, -1, 2, &s, &n) && n == 5 && std::memcmp(s, "hello", 5) == 0);
  CHECK(!lt::Replay::field(l1, -1, 8, &s, &n));
  // empty fields: not available
  CHECK(rp.parse("0,1794101400080,,35.3949090,140.0880580,,3,", -1, &r, &why));
  CHECK(!r.row.speedOk && !r.row.rpmOk && !r.fix.valid && r.badFields == 0);
  // unreadable fields are counted and left out
  CHECK(rp.parse("1,abc,,35.3949090,140.0880580,12.5,3,55x0", -1, &r, &why));
  CHECK(r.badFields == 2 && !r.row.utcOk && !r.row.rpmOk && r.row.speedOk && !r.fix.valid);
  // a line cut by a power loss, an empty line, the header again
  CHECK(!rp.parse("3,1794101400120,x,35.39", -1, &r, &why) && std::strcmp(why, "columns") == 0);
  CHECK(!rp.parse("\r\n", -1, &r, &why) && std::strcmp(why, "empty") == 0);
  CHECK(!rp.parse("utc_ms,lat,lon,speed_kmh,fix", -1, &r, &why) && std::strcmp(why, "header") == 0);
  // a needed column missing
  lt::Replay rp2;
  CHECK(!rp2.header("utc_ms,lat,lon,fix") && !rp2.ready());
  CHECK(!rp2.parse(l1, -1, &r, &why) && std::strcmp(why, "no-header") == 0);
  // the full main log row comes back with every value (at the precision written)
  lt::Replay rp3;
  CHECK(rp3.header(lt::csvlog::kMainHeader));
  const char* full =
      "1794101400040,35.3949090,140.0880580,45.2,123.46,87.1,0.85,18,3,0.512,-0.235,0.000,-12.3,5500,85.5,121.00,"
      "88.0,62.5,0.35,R,2,1,12345,-0.123\n";
  CHECK(rp3.parse(full, -1, &r, &why));
  const lt::csvlog::MainRow& m = r.row;
  CHECK(m.altOk && m.altM == 45.2f && m.headingOk && m.headingDeg == 87.1f && m.haccOk && m.haccM == 0.85f);
  CHECK(m.satsOk && m.sats == 18 && m.fixOk && m.fix == 3 && m.imuOk && m.lonG == 0.512f && m.latG == -0.235f &&
        m.vertG == 0.0f);
  CHECK(m.yawOk && m.yawDps == -12.3f && m.throttleOk && m.throttlePct == 85.5f && m.speedCanOk &&
        m.speedCanKmh == 121.0f);
  CHECK(m.waterOk && m.waterC == 88.0f && m.fuelOk && m.fuelPct == 62.5f && m.boostOk && m.boostBar == 0.35f);
  CHECK(m.gearOk && m.gear == 'R' && m.lap == 2 && m.sector == 1 && m.lapMsOk && m.lapMs == 12345 && m.ghostOk &&
        m.ghostDeltaS == -0.123f);
  // written again it is the same text
  char b[512];
  CHECK(lt::csvlog::mainLine(b, sizeof b, m) > 0 && std::strcmp(b, full) == 0);
}
