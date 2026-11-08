// core/csvlog: exact text of the session files (ARCH §5.1-§5.3).
#include <cmath>
#include <cstring>
#include <string>

#include "core/csvlog.h"
#include "core/lap.h"
#include "core/maint.h"
#include "tmini.h"

namespace {

std::string show(const char* s) {
  std::string o;
  for (const char* p = s; *p; ++p) o += *p == '\n' ? std::string("\\n") : std::string(1, *p);
  return o;
}

// 2026-11-08 01:30:00.040 UTC (10:30 JST)
const int64_t kUtc = 1794101400040ll;

lt::csvlog::MainRow fullRow() {
  lt::csvlog::MainRow r;
  r.utcOk = true;
  r.utcMs = kUtc;
  r.posOk = true;
  r.latE7 = 353949090;
  r.lonE7 = 1400880580;
  r.altOk = true;
  r.altM = 45.24f;
  r.speedOk = true;
  r.speedKmh = 123.456f;
  r.headingOk = true;
  r.headingDeg = 87.12f;
  r.haccOk = true;
  r.haccM = 0.854f;
  r.satsOk = true;
  r.sats = 18;
  r.fixOk = true;
  r.fix = 3;
  r.imuOk = true;
  r.lonG = 0.5123f;
  r.latG = -0.2346f;
  r.vertG = 0.0004f;
  r.yawOk = true;
  r.yawDps = -12.34f;
  r.rpmOk = true;
  r.rpm = 5500;
  r.throttleOk = true;
  r.throttlePct = 85.54f;
  r.speedCanOk = true;
  r.speedCanKmh = 121.0f;
  r.waterOk = true;
  r.waterC = 88.04f;
  r.fuelOk = true;
  r.fuelPct = 62.53f;
  r.boostOk = true;
  r.boostBar = 0.354f;
  r.gearOk = true;
  r.gear = '3';
  r.lap = 2;
  r.sector = 1;
  r.lapMsOk = true;
  r.lapMs = 12345;
  r.ghostOk = true;
  r.ghostDeltaS = -0.1234f;
  return r;
}

const char kFullLine[] =
    "1794101400040,35.3949090,140.0880580,45.2,123.46,87.1,0.85,18,3,0.512,-0.235,0.000,-12.3,5500,85.5,121.00,"
    "88.0,62.5,0.35,3,2,1,12345,-0.123\n";

// every capacity below length + 1 gives 0, length + 1 gives the text
template <typename F>
int capacityErrors(F write, int len) {
  int bad = 0;
  for (int cap = 0; cap <= len + 1; ++cap) {
    char b[1024];
    std::memset(b, 'x', sizeof b);
    const int n = write(b, cap);
    if (cap <= len && n != 0) ++bad;
    if (cap == len + 1 && n != len) ++bad;
  }
  return bad;
}

}  // namespace

TEST(csvlog_main_header_and_rows_exact) {
  char b[512];
  const char* arch =  // ARCH §5.2, copied
      "utc_ms,lat,lon,alt_m,speed_kmh,heading_deg,hacc_m,sats,fix,lon_g,lat_g,vert_g,yaw_dps,rpm,throttle_pct,"
      "speed_can_kmh,water_c,fuel_pct,boost_bar,gear,lap,sector,lap_ms,ghost_delta_s\n";
  CHECK(lt::csvlog::mainHeader(b, sizeof b) == static_cast<int>(std::strlen(arch)) && std::strcmp(b, arch) == 0);
  int n = lt::csvlog::mainLine(b, sizeof b, fullRow());
  CHECKF(n == static_cast<int>(std::strlen(kFullLine)) && std::strcmp(b, kFullLine) == 0, "got %s", show(b).c_str());
  // nothing valid: empty fields, lap and sector 0
  lt::csvlog::MainRow empty;
  n = lt::csvlog::mainLine(b, sizeof b, empty);
  CHECKF(std::strcmp(b, ",,,,,,,,,,,,,,,,,,,,0,0,,\n") == 0, "got %s", show(b).c_str());
  // south / west, rounding of negatives, other gears
  lt::csvlog::MainRow r = fullRow();
  r.latE7 = -337000000;
  r.lonE7 = -706000001;
  r.altM = -3.06f;
  r.gear = 'R';
  r.ghostDeltaS = 0.0006f;
  r.imuOk = false;
  r.lapMsOk = false;
  r.speedKmh = 0.0f;
  lt::csvlog::mainLine(b, sizeof b, r);
  CHECKF(std::strcmp(b, "1794101400040,-33.7000000,-70.6000001,-3.1,0.00,87.1,0.85,18,3,,,,-12.3,5500,85.5,121.00,"
                        "88.0,62.5,0.35,R,2,1,,0.001\n") == 0,
         "got %s", show(b).c_str());
  r.gear = '6';  // not a gear of the 595
  lt::csvlog::mainLine(b, sizeof b, r);
  CHECK(std::strstr(b, ",0.35,,2,1,") != nullptr);
  r.gear = 'N';
  r.waterC = NAN;  // not finite: empty although waterOk
  lt::csvlog::mainLine(b, sizeof b, r);
  CHECK(std::strstr(b, ",121.00,,62.5,0.35,N,") != nullptr);
  // buffer sizes
  const lt::csvlog::MainRow full = fullRow();
  CHECK(capacityErrors([&](char* buf, int cap) { return lt::csvlog::mainLine(buf, cap, full); },
                       static_cast<int>(std::strlen(kFullLine))) == 0);
  CHECK(capacityErrors([&](char* buf, int cap) { return lt::csvlog::mainHeader(buf, cap); },
                       static_cast<int>(std::strlen(arch))) == 0);
}

TEST(csvlog_fix_of_row) {
  lt::csvlog::MainRow r = fullRow();
  lt::Fix f = lt::csvlog::fixOf(r);
  CHECK(f.tUs == kUtc * 1000 && f.latE7 == 353949090 && f.lonE7 == 1400880580 && f.valid);
  CHECK(f.speedMps == 12346.0f / 360.0f);  // the speed as written: 123.46 km/h
  r.fix = 2;
  CHECK(!lt::csvlog::fixOf(r).valid);
  r = fullRow();
  r.posOk = false;
  CHECK(!lt::csvlog::fixOf(r).valid);
  r = fullRow();
  r.speedOk = false;
  CHECK(!lt::csvlog::fixOf(r).valid && lt::csvlog::fixOf(r).speedMps == 0.0f);
  r = fullRow();
  r.utcOk = false;
  CHECK(!lt::csvlog::fixOf(r).valid);
}

TEST(csvlog_laps_imu_can_lines) {
  char b[256];
  CHECK(lt::csvlog::lapsHeader(b, sizeof b) > 0 && std::strcmp(b, "lap,start_utc_ms,end_utc_ms,lap_ms,sectors_ms\n") == 0);
  const int64_t sec[4] = {22649, 25044, 17301, 17671};
  lt::csvlog::lapsLine(b, sizeof b, 3, kUtc, kUtc + 82665, 82665, sec, 4);
  CHECKF(std::strcmp(b, "3,1794101400040,1794101482705,82665,22649;25044;17301;17671\n") == 0, "got %s", show(b).c_str());
  lt::csvlog::lapsLine(b, sizeof b, 1, kUtc, kUtc + 90000, 90000, nullptr, 0);
  CHECK(std::strcmp(b, "1,1794101400040,1794101490040,90000,\n") == 0);
  lt::LapResult r{};
  r.lapNo = 3;
  r.startUs = kUtc * 1000 + 123;
  r.lapUs = 82665432;
  r.endUs = r.startUs + r.lapUs;
  r.numSplits = 4;
  const int64_t split[4] = {22649499, 25044500, 17301000, 17670433};
  for (int i = 0; i < 4; ++i) r.splitUs[i] = split[i];
  lt::csvlog::lapsLine(b, sizeof b, r);
  CHECKF(std::strcmp(b, "3,1794101400040,1794101482706,82665,22649;25045;17301;17670\n") == 0, "got %s", show(b).c_str());
  r.numSplits = 0;  // a sector line was missed
  lt::csvlog::lapsLine(b, sizeof b, r);
  CHECK(std::strcmp(b, "3,1794101400040,1794101482706,82665,\n") == 0);

  CHECK(lt::csvlog::imuHeader(b, sizeof b) > 0 && std::strcmp(b, "t_ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps\n") == 0);
  const lt::csvlog::ImuRow imu = {kUtc + 5, 0.01234f, -0.98764f, 1.00004f, 1.234f, -0.004f, 123.456f};
  lt::csvlog::imuLine(b, sizeof b, imu);
  CHECKF(std::strcmp(b, "1794101400045,0.0123,-0.9876,1.0000,1.23,0.00,123.46\n") == 0, "got %s", show(b).c_str());

  CHECK(lt::csvlog::canHeader(b, sizeof b) > 0 && std::strcmp(b, "t_ms,bus,id,dlc,data\n") == 0);
  const uint8_t d[12] = {0x01, 0xFE, 0x1C, 0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22};
  lt::csvlog::canLine(b, sizeof b, 730814, 'C', 0x0810A000u, 4, d);  // a frame of the round gauge's CAN log
  CHECKF(std::strcmp(b, "730814,C,0810A000,4,01FE1C00\n") == 0, "got %s", show(b).c_str());
  lt::csvlog::canLine(b, sizeof b, 1, 'B', 0x123u, 0, d);
  CHECK(std::strcmp(b, "1,B,00000123,0,\n") == 0);
  lt::csvlog::canLine(b, sizeof b, 2, 'C', 0x1FFFFFFFu, 12, d);  // at most 8 bytes
  CHECK(std::strcmp(b, "2,C,1FFFFFFF,12,01FE1C00AABBCCDD\n") == 0);
  CHECK(capacityErrors([&](char* buf, int cap) { return lt::csvlog::canLine(buf, cap, 730814, 'C', 0x0810A000u, 4, d); },
                       29) == 0);
  CHECK(capacityErrors([&](char* buf, int cap) { return lt::csvlog::lapsLine(buf, cap, r); }, 37) == 0);
}

TEST(csvlog_meta_json_exact) {
  lt::csvlog::Meta m;
  m.id = "20261108_103000";
  m.fw = "0.1.0";
  m.tzMin = 540;
  m.startUtcMs = 1794101400000ll;
  m.endUtcMs = 1794103200000ll;
  m.courseId = "sodegaura";
  m.courseName = "袖ヶ浦フォレストレースウェイ";
  m.laps = 7;
  m.bestLap = 3;
  m.bestMs = 82665;
  m.maxKmhOk = true;
  m.maxKmh = 176.4f;
  m.maxRpmOk = true;
  m.maxRpm = 6550;
  m.closed = true;
  char b[1024];
  int n = lt::csvlog::metaJson(b, sizeof b, m);
  const char* expect =
      "{\n"
      "  \"id\": \"20261108_103000\",\n"
      "  \"format\": 1,\n"
      "  \"fw\": \"0.1.0\",\n"
      "  \"tz_min\": 540,\n"
      "  \"start_utc_ms\": 1794101400000,\n"
      "  \"end_utc_ms\": 1794103200000,\n"
      "  \"course\": {\"id\": \"sodegaura\", \"name\": \"袖ヶ浦フォレストレースウェイ\"},\n"
      "  \"laps\": 7,\n"
      "  \"best_lap\": 3,\n"
      "  \"best_ms\": 82665,\n"
      "  \"max_kmh\": 176.4,\n"
      "  \"max_rpm\": 6550,\n"
      "  \"closed\": true,\n"
      "  \"files\": {\"csv\": \"20261108_103000_sodegaura.csv\", \"vbo\": \"20261108_103000_sodegaura.vbo\","
      " \"ubx\": \"20261108_103000_sodegaura_raw.ubx\", \"imu\": \"20261108_103000_sodegaura_imu.csv\","
      " \"can\": \"20261108_103000_sodegaura_can.csv\", \"laps\": \"20261108_103000_sodegaura_laps.csv\"}\n"
      "}\n";
  CHECKF(n == static_cast<int>(std::strlen(expect)) && std::strcmp(b, expect) == 0, "got:\n%s", b);
  // street, open, no laps yet
  lt::csvlog::Meta s;
  s.id = "20261108_080000";
  s.fw = "0.1.0";
  s.startUtcMs = 1;
  s.maxKmhOk = true;
  s.maxKmh = 60.0f;
  n = lt::csvlog::metaJson(b, sizeof b, s);
  CHECK(n > 0 && std::strstr(b, "\"course\": null,") && std::strstr(b, "\"best_lap\": null,") &&
        std::strstr(b, "\"best_ms\": null,") && std::strstr(b, "\"max_kmh\": 60.0,") && std::strstr(b, "\"max_rpm\": null,") &&
        std::strstr(b, "\"closed\": false,") && std::strstr(b, "\"csv\": \"20261108_080000_street.csv\""));
  CHECK(capacityErrors([&](char* buf, int cap) { return lt::csvlog::metaJson(buf, cap, m); },
                       static_cast<int>(std::strlen(expect))) == 0);
}

TEST(csvlog_session_id_and_file_names) {
  char b[64];
  CHECK(lt::csvlog::sessionId(b, sizeof b, kUtc, 540) == 15 && std::strcmp(b, "20261108_103000") == 0);
  CHECK(lt::csvlog::sessionId(b, sizeof b, kUtc, 0) == 15 && std::strcmp(b, "20261108_013000") == 0);
  CHECK(lt::csvlog::sessionId(b, sizeof b, 0, -300) == 15 && std::strcmp(b, "19691231_190000") == 0);
  // 23:59:59.999 JST stays on that day
  const int64_t endOfDay = static_cast<int64_t>(lt::daysFromCivil(2026, 11, 8)) * 86400000 + 86399999 - 9 * 3600000;
  CHECK(lt::csvlog::sessionId(b, sizeof b, endOfDay, 540) == 15 && std::strcmp(b, "20261108_235959") == 0);
  CHECK(lt::csvlog::sessionId(b, 15, kUtc, 540) == 0);
  const char* id = "20261108_103000";
  struct K {
    lt::csvlog::FileKind kind;
    const char* name;
  };
  const K kinds[] = {{lt::csvlog::FileKind::Csv, "20261108_103000_sodegaura.csv"},
                     {lt::csvlog::FileKind::Vbo, "20261108_103000_sodegaura.vbo"},
                     {lt::csvlog::FileKind::Ubx, "20261108_103000_sodegaura_raw.ubx"},
                     {lt::csvlog::FileKind::Imu, "20261108_103000_sodegaura_imu.csv"},
                     {lt::csvlog::FileKind::Can, "20261108_103000_sodegaura_can.csv"},
                     {lt::csvlog::FileKind::Laps, "20261108_103000_sodegaura_laps.csv"}};
  for (const K& k : kinds) {
    CHECK(lt::csvlog::fileName(b, sizeof b, id, "sodegaura", k.kind) > 0 && std::strcmp(b, k.name) == 0);
  }
  CHECK(lt::csvlog::fileName(b, sizeof b, id, nullptr, lt::csvlog::FileKind::Vbo) > 0 &&
        std::strcmp(b, "20261108_103000_street.vbo") == 0);
  CHECK(lt::csvlog::fileName(b, 20, id, "sodegaura", lt::csvlog::FileKind::Csv) == 0);
}
