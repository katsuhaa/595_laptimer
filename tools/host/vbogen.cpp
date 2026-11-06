// tools/host/vbogen.cpp - writes the test VBO for the RaceChrono import check (go.sh vbo).
// 2 minutes at 25 Hz on a small synthetic oval placed on Sodegaura Forest Raceway, with two
// extra channels (rpm, throttle). The file is read back and checked before PASS is printed.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "core/geo.h"
#include "core/vbo.h"
#include "test/synth.h"

namespace {

constexpr int32_t kLat0 = 353940000;   // 35.394 N
constexpr int32_t kLon0 = 1400890000;  // 140.089 E
constexpr int kRows = 3000;            // 120 s at 25 Hz
constexpr uint32_t kStartUtcMs = 1u * 3600u * 1000u;  // 01:00:00.000 UTC = 10:00 JST

bool fail(const char* what) {
  std::printf("FAIL: %s\n", what);
  return false;
}

// Reads the file back and checks what RaceChrono needs.
bool verify(const char* path) {
  FILE* fp = std::fopen(path, "rb");
  if (!fp) return fail("cannot read the file back");
  std::string all;
  char chunk[65536];
  size_t n = 0;
  while ((n = std::fread(chunk, 1, sizeof chunk, fp)) > 0) all.append(chunk, n);
  std::fclose(fp);

  for (size_t i = 0; i < all.size(); ++i) {
    if (all[i] == '\n' && (i == 0 || all[i - 1] != '\r')) return fail("line without CR LF");
  }
  const size_t data = all.find("[data]\r\n");
  if (data == std::string::npos) return fail("[data] section missing");

  int rows = 0;
  long prevMs = -1;
  double latMin = 1e9, latMax = -1e9, lonMin = 1e9, lonMax = -1e9;
  size_t pos = data + 8;
  while (pos < all.size()) {
    const size_t end = all.find("\r\n", pos);
    if (end == std::string::npos) break;
    const std::string line = all.substr(pos, end - pos);
    pos = end + 2;
    int sats = 0;
    char tm[16] = {0};
    char lat[16] = {0};
    char lon[16] = {0};
    if (std::sscanf(line.c_str(), "%d %15s %15s %15s", &sats, tm, lat, lon) != 4) {
      return fail("data line does not parse");
    }
    if (lon[0] != '-') return fail("longitude is not negative (Japan must be negative)");
    const long hh = (tm[0] - '0') * 10 + (tm[1] - '0');
    const long mm = (tm[2] - '0') * 10 + (tm[3] - '0');
    const double ss = std::atof(tm + 4);
    const long ms = std::lround(((hh * 60 + mm) * 60 + ss) * 1000.0);
    if (prevMs >= 0 && ms - prevMs != 40) return fail("time step is not 40 ms");
    prevMs = ms;
    const double la = std::atof(lat) / 60.0;
    const double lo = -std::atof(lon) / 60.0;  // back to east positive
    latMin = std::fmin(latMin, la);
    latMax = std::fmax(latMax, la);
    lonMin = std::fmin(lonMin, lo);
    lonMax = std::fmax(lonMax, lo);
    ++rows;
  }
  if (rows != kRows) return fail("wrong number of data lines");
  if (latMin < 35.38 || latMax > 35.41 || lonMin < 140.07 || lonMax > 140.11) {
    return fail("positions are not at Sodegaura");
  }
  std::printf("check: %d data lines, 25 Hz (40 ms steps), CR LF, longitude negative\n", rows);
  std::printf("check: latitude %.5f .. %.5f N, longitude %.5f .. %.5f E\n", latMin, latMax, lonMin,
              lonMax);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "out/test_sodegaura_synth.vbo";

  lt::Plane pl;
  pl.init(kLat0, kLon0);
  synth::Track tr;  // small oval: 300 m straights, 80 m radius (about 1.1 km)
  tr.straight = 300.0;
  tr.radius = 80.0;
  tr.cornerSpeed = 20.0;
  tr.straightLo = 24.0;
  tr.straightHi = 34.0;
  synth::RunConfig cfg;
  cfg.laps = 3;
  cfg.sfS = 150.0;
  cfg.noiseM = 0.0;
  cfg.lapScaleMin = 1.0;
  cfg.lapScaleMax = 1.0;
  cfg.t0Us = 0;
  const synth::Run run = synth::makeRun(tr, pl, cfg);
  if (static_cast<int>(run.fixes.size()) < kRows) {
    std::printf("FAIL: synthetic run too short\n");
    return 1;
  }

  FILE* fp = std::fopen(path, "wb");
  if (!fp) {
    std::printf("FAIL: cannot write %s\n", path);
    return 1;
  }
  const std::time_t now = std::time(nullptr);
  const std::tm* tmNow = std::localtime(&now);
  const lt::VboDateTime created = {tmNow->tm_year + 1900, tmNow->tm_mon + 1, tmNow->tm_mday,
                                   tmNow->tm_hour,        tmNow->tm_min,      tmNow->tm_sec};
  const lt::VboChannel ch[2] = {{"rpm", "rpm"}, {"throttle", "%"}};
  char buf[2048];
  int n = lt::vboHeader(buf, sizeof buf, created, ch, 2,
                        "595_laptimer synthetic test data (not a real session)");
  std::fwrite(buf, 1, static_cast<size_t>(n), fp);

  // rows are re-timed to exact 40 ms steps from 01:00:00.000 UTC
  const uint32_t t0 = kStartUtcMs;
  for (int i = 0; i < kRows; ++i) {
    const lt::Fix& f = run.fixes[static_cast<size_t>(i)];
    const double s = run.fixS[static_cast<size_t>(i)];
    const float kmh = f.speedMps * 3.6f;
    const float rpm = 2500.0f + (kmh - 60.0f) * 60.0f;
    const float throttle = tr.inCorner(s) ? 30.0f : 100.0f;
    const float extra[2] = {rpm, throttle};
    lt::VboSample v{};
    v.sats = 14;
    v.utcMsOfDay = t0 + static_cast<uint32_t>(i) * 40u;
    v.latE7 = f.latE7;
    v.lonE7 = f.lonE7;
    v.speedKmh = kmh;
    v.headingDeg = static_cast<float>(tr.headingAt(s));
    v.heightM = 40.0f;
    v.extra = extra;
    n = lt::vboLine(buf, sizeof buf, v, 2);
    std::fwrite(buf, 1, static_cast<size_t>(n), fp);
  }
  std::fclose(fp);

  std::printf("wrote %s (%d rows, %.0f s, %.0f m oval at Sodegaura, start 01:00:00 UTC)\n", path,
              kRows, kRows * 0.04, tr.length());
  if (!verify(path)) {
    std::printf("RESULT: FAIL\n");
    return 1;
  }
  std::printf("RESULT: PASS\n");
  return 0;
}
