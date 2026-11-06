// tools/host/vbogen.cpp - writes VBO files for the RaceChrono import check (go.sh vbo).
//   vbogen <out.vbo>              2 minutes at 25 Hz on a small synthetic oval placed on
//                                 Sodegaura Forest Raceway, with rpm and throttle channels
//   vbogen --rcz <dir> <out.vbo>  a RaceChrono session (unzipped .rcz) converted to VBO,
//                                 GPS channels only, so RaceChrono can time it again
// Each file is read back and checked; the last line is "PASS <file>" or "FAIL <file>: ...".
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
#include "tools/host/rcz.h"

namespace {

constexpr int32_t kLat0 = 353940000;   // 35.394 N
constexpr int32_t kLon0 = 1400890000;  // 140.089 E

struct Expect {
  int rows;
  int stepMs;  // 0: only check that the time increases
  double latMin, latMax, lonMin, lonMax;
};

bool fail(const char* path, const char* what) {
  std::printf("FAIL %s: %s\n", path, what);
  return false;
}

// Reads the file back and checks what RaceChrono needs.
bool verify(const char* path, const Expect& e) {
  FILE* fp = std::fopen(path, "rb");
  if (!fp) return fail(path, "cannot read the file back");
  std::string all;
  char chunk[65536];
  size_t n = 0;
  while ((n = std::fread(chunk, 1, sizeof chunk, fp)) > 0) all.append(chunk, n);
  std::fclose(fp);

  for (size_t i = 0; i < all.size(); ++i) {
    if (all[i] == '\n' && (i == 0 || all[i - 1] != '\r')) return fail(path, "line without CR LF");
  }
  const size_t data = all.find("[data]\r\n");
  if (data == std::string::npos) return fail(path, "[data] section missing");

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
      return fail(path, "data line does not parse");
    }
    if (lon[0] != '-') return fail(path, "longitude is not negative (Japan must be negative)");
    const long hh = (tm[0] - '0') * 10 + (tm[1] - '0');
    const long mm = (tm[2] - '0') * 10 + (tm[3] - '0');
    const double ss = std::atof(tm + 4);
    const long ms = std::lround(((hh * 60 + mm) * 60 + ss) * 1000.0);
    if (prevMs >= 0) {
      if (e.stepMs > 0 && ms - prevMs != e.stepMs) return fail(path, "time step is not constant");
      if (e.stepMs == 0 && ms <= prevMs) return fail(path, "time does not increase");
    }
    prevMs = ms;
    const double la = std::atof(lat) / 60.0;
    const double lo = -std::atof(lon) / 60.0;  // back to east positive
    latMin = std::fmin(latMin, la);
    latMax = std::fmax(latMax, la);
    lonMin = std::fmin(lonMin, lo);
    lonMax = std::fmax(lonMax, lo);
    ++rows;
  }
  if (rows != e.rows) return fail(path, "wrong number of data lines");
  if (latMin < e.latMin || latMax > e.latMax || lonMin < e.lonMin || lonMax > e.lonMax) {
    return fail(path, "positions are outside the expected area");
  }
  std::printf("check: %d data lines, CR LF, longitude negative, time increasing%s\n", rows,
              e.stepMs ? " in equal steps" : "");
  std::printf("check: latitude %.5f .. %.5f N, longitude %.5f .. %.5f E\n", latMin, latMax, lonMin,
              lonMax);
  std::printf("PASS %s\n", path);
  return true;
}

lt::VboDateTime utcOf(int64_t epochMs) {
  const std::time_t t = static_cast<std::time_t>(epochMs / 1000);
  const std::tm* g = std::gmtime(&t);
  return {g->tm_year + 1900, g->tm_mon + 1, g->tm_mday, g->tm_hour, g->tm_min, g->tm_sec};
}

int synthetic(const char* path) {
  constexpr int kRows = 3000;                            // 120 s at 25 Hz
  constexpr uint32_t kStartUtcMs = 1u * 3600u * 1000u;  // 01:00:00.000 UTC = 10:00 JST
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
  cfg.lapScaleMin = 1.0;
  cfg.lapScaleMax = 1.0;
  cfg.t0Us = 0;
  const synth::Run run = synth::makeRun(tr, pl, cfg);
  if (static_cast<int>(run.fixes.size()) < kRows) {
    fail(path, "synthetic run too short");
    return 1;
  }

  FILE* fp = std::fopen(path, "wb");
  if (!fp) {
    fail(path, "cannot write");
    return 1;
  }
  const std::time_t now = std::time(nullptr);
  lt::VboDateTime created = utcOf(static_cast<int64_t>(now) * 1000);
  created.hour = 1;  // the date of today with the time of the first row
  created.minute = 0;
  created.second = 0;
  const lt::VboChannel ch[2] = {{"rpm", "rpm"}, {"throttle", "%"}};
  char buf[2048];
  int n = lt::vboHeader(buf, sizeof buf, created, ch, 2,
                        "595_laptimer synthetic test data (not a real session)");
  std::fwrite(buf, 1, static_cast<size_t>(n), fp);
  for (int i = 0; i < kRows; ++i) {
    const lt::Fix& f = run.fixes[static_cast<size_t>(i)];
    const double s = run.fixS[static_cast<size_t>(i)];
    const float kmh = f.speedMps * 3.6f;
    const float extra[2] = {2500.0f + (kmh - 60.0f) * 60.0f, tr.inCorner(s) ? 30.0f : 100.0f};
    lt::VboSample v{};
    v.sats = 14;
    v.utcMsOfDay = kStartUtcMs + static_cast<uint32_t>(i) * 40u;  // exact 40 ms steps
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
  std::printf("wrote %s (%d rows, 120 s, %.0f m oval at Sodegaura, start 01:00:00 UTC)\n", path,
              kRows, tr.length());
  const Expect e = {kRows, 40, 35.38, 35.41, 140.07, 140.11};
  return verify(path, e) ? 0 : 1;
}

int fromRcz(const char* dir, const char* path) {
  rcz::Session s;
  std::string err;
  if (!rcz::load(dir, &s, &err)) {
    fail(path, err.c_str());
    return 1;
  }
  FILE* fp = std::fopen(path, "wb");
  if (!fp) {
    fail(path, "cannot write");
    return 1;
  }
  std::string ascii;  // VBO text stays ASCII: drop the track name in Japanese from the file name
  for (char c : s.name) {
    if (static_cast<unsigned char>(c) < 0x80) ascii += c;
  }
  while (!ascii.empty() && ascii.back() == '_') ascii.pop_back();
  const std::string comment = "595_laptimer: RaceChrono " + ascii + " converted to VBO";
  char buf[2048];
  int n = lt::vboHeader(buf, sizeof buf, utcOf(s.tMs.front()), nullptr, 0, comment.c_str());
  std::fwrite(buf, 1, static_cast<size_t>(n), fp);
  double latMin = 1e9, latMax = -1e9, lonMin = 1e9, lonMax = -1e9;
  for (size_t i = 0; i < s.tMs.size(); ++i) {
    lt::VboSample v{};
    v.sats = s.sats.empty() ? 0 : s.sats[i];
    v.utcMsOfDay = static_cast<uint32_t>(s.tMs[i] % 86400000);
    v.latE7 = s.latE7[i];
    v.lonE7 = s.lonE7[i];
    v.speedKmh = s.speedMps[i] * 3.6f;
    v.headingDeg = s.bearingDeg[i];
    v.heightM = s.altM[i];
    n = lt::vboLine(buf, sizeof buf, v, 0);
    std::fwrite(buf, 1, static_cast<size_t>(n), fp);
    latMin = std::fmin(latMin, s.latE7[i] * 1e-7);
    latMax = std::fmax(latMax, s.latE7[i] * 1e-7);
    lonMin = std::fmin(lonMin, s.lonE7[i] * 1e-7);
    lonMax = std::fmax(lonMax, s.lonE7[i] * 1e-7);
  }
  std::fclose(fp);
  int finished = 0;
  for (const rcz::Lap& l : s.laps) finished += l.finished ? 1 : 0;
  std::printf("wrote %s (%zu rows, %d laps in RaceChrono)\n", path, s.tMs.size(), finished);
  const Expect e = {static_cast<int>(s.tMs.size()), 0, latMin - 1e-5, latMax + 1e-5,
                    lonMin - 1e-5, lonMax + 1e-5};
  return verify(path, e) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2) return synthetic(argv[1]);
  if (argc == 4 && std::strcmp(argv[1], "--rcz") == 0) return fromRcz(argv[2], argv[3]);
  std::printf("usage: vbogen <out.vbo> | vbogen --rcz <unzipped .rcz directory> <out.vbo>\n");
  return 2;
}
