#include <cstdio>
#include <cstring>
#include <string>

#include "core/vbo.h"
#include "tmini.h"

namespace {

std::string show(const char* s) {  // CR LF made visible for messages
  std::string o;
  for (const char* p = s; *p; ++p) {
    if (*p == '\r') o += "\\r";
    else if (*p == '\n') o += "\\n";
    else o += *p;
  }
  return o;
}

const lt::VboChannel kCh[2] = {{"Engine RPM", "rpm"}, {"throttle", "%"}};

}  // namespace

TEST(vbo_header_exact) {
  char buf[1024];
  const lt::VboDateTime c = {2026, 11, 6, 10, 0, 0};
  const int n = lt::vboHeader(buf, sizeof buf, c, kCh, 2, "595 laptimer");
  const char* expect =
      "File created on 06/11/2026 @ 10:00:00\r\n\r\n"
      "[header]\r\nsatellites\r\ntime\r\nlatitude\r\nlongitude\r\nvelocity kmh\r\nheading\r\n"
      "height\r\nEngine RPM\r\nthrottle\r\n\r\n"
      "[channel units]\r\nrpm\r\n%\r\n\r\n"
      "[comments]\r\n595 laptimer\r\n\r\n"
      "[column names]\r\nsats time lat long velocity heading height Engine_RPM throttle\r\n\r\n"
      "[data]\r\n";
  CHECKF(n == static_cast<int>(std::strlen(expect)), "length %d", n);
  CHECKF(std::strcmp(buf, expect) == 0, "got: %s", show(buf).c_str());
}

TEST(vbo_line_exact_japan_longitude_negative) {
  char buf[256];
  const float extra[2] = {6450.0f, 85.5f};
  lt::VboSample s{};
  s.sats = 14;
  s.utcMsOfDay = ((1 * 60 + 32) * 60 + 5) * 1000 + 40;  // 01:32:05.040 UTC
  s.latE7 = 353938900;    // 35.39389 N
  s.lonE7 = 1400888900;   // 140.08889 E -> west-positive minutes are negative
  s.speedKmh = 128.345f;
  s.headingDeg = 87.12f;
  s.heightM = 45.2f;
  s.extra = extra;
  lt::vboLine(buf, sizeof buf, s, 2);
  const char* expect =
      "014 013205.040 +02123.63340 -08405.33340 128.345 087.12 +00045.20 +6.45000E+03"
      " +8.55000E+01\r\n";
  CHECKF(std::strcmp(buf, expect) == 0, "got: %s", show(buf).c_str());
}

TEST(vbo_line_signs_and_wraps) {
  char buf[256];
  lt::VboSample s{};
  s.sats = 7;
  s.utcMsOfDay = 86399999;      // 23:59:59.999
  s.latE7 = -337000000;         // 33.7 S
  s.lonE7 = -960000000;         // 96.0 W -> positive
  s.speedKmh = -1.0f;           // clamped to 0
  s.headingDeg = 359.999f;      // rounds to 360.00 -> 000.00
  s.heightM = -3.5f;
  lt::vboLine(buf, sizeof buf, s, 0);
  const char* expect = "007 235959.999 -02022.00000 +05760.00000 000.000 000.00 -00003.50\r\n";
  CHECKF(std::strcmp(buf, expect) == 0, "got: %s", show(buf).c_str());
}

TEST(vbo_minutes_round_trip) {
  // every 1e-7 deg step near the origin decodes back within half a VBO digit (0.93 cm)
  char buf[256];
  for (int32_t e7 = 353940000; e7 < 353940000 + 2000; e7 += 7) {
    lt::VboSample s{};
    s.latE7 = e7;
    s.lonE7 = 1400890000 + (e7 - 353940000);
    lt::vboLine(buf, sizeof buf, s, 0);
    double lat = 0.0;
    double lon = 0.0;
    std::sscanf(buf + 15, "%lf %lf", &lat, &lon);
    const double errLat = lat / 60.0 * 1e7 - e7;
    const double errLon = -lon / 60.0 * 1e7 - s.lonE7;
    CHECKF(errLat > -0.84 && errLat < 0.84, "lat %d -> %s", e7, show(buf).c_str());
    CHECKF(errLon > -0.84 && errLon < 0.84, "lon %d -> %s", s.lonE7, show(buf).c_str());
  }
}

TEST(vbo_buffer_too_small) {
  char buf[16];
  lt::VboSample s{};
  CHECK(lt::vboLine(buf, sizeof buf, s, 0) == -1);
  const lt::VboDateTime c = {2026, 1, 1, 0, 0, 0};
  CHECK(lt::vboHeader(buf, sizeof buf, c, nullptr, 0, nullptr) == -1);
}
