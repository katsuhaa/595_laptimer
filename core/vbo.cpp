#include "vbo.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace lt {

namespace {

// Appends formatted text; on overflow marks *len = -1 and stays there.
void put(char* buf, int cap, int* len, const char* fmt, ...) {
  if (*len < 0) return;
  va_list ap;
  va_start(ap, fmt);
  const int n = std::vsnprintf(buf + *len, static_cast<size_t>(cap - *len), fmt, ap);
  va_end(ap);
  if (n < 0 || n >= cap - *len) {
    *len = -1;
    return;
  }
  *len += n;
}

// Column name: the long name with spaces replaced by '_'
void putColumnName(char* buf, int cap, int* len, const char* name) {
  for (const char* p = name; *p; ++p) put(buf, cap, len, "%c", *p == ' ' ? '_' : *p);
}

// Degrees (1e-7) to signed minutes with 5 decimals, integer math only: "+02123.63340"
void putMinutes(char* buf, int cap, int* len, int64_t e7) {
  const int64_t q = e7 * 6;  // 1e-6 minutes
  const int64_t v = q >= 0 ? (q + 5) / 10 : -((-q + 5) / 10);  // 1e-5 minutes, rounded
  const int64_t m = v < 0 ? -v : v;
  put(buf, cap, len, "%c%05d.%05d", v < 0 ? '-' : '+', static_cast<int>(m / 100000),
      static_cast<int>(m % 100000));
}

}  // namespace

int vboHeader(char* buf, int cap, const VboDateTime& c, const VboChannel* ch, int numCh,
              const char* comment) {
  int len = 0;
  if (cap <= 0) return -1;
  buf[0] = '\0';
  put(buf, cap, &len, "File created on %02d/%02d/%04d @ %02d:%02d:%02d\r\n\r\n", c.day, c.month,
      c.year, c.hour, c.minute, c.second);
  put(buf, cap, &len,
      "[header]\r\nsatellites\r\ntime\r\nlatitude\r\nlongitude\r\nvelocity kmh\r\nheading\r\n"
      "height\r\n");
  for (int i = 0; i < numCh; ++i) put(buf, cap, &len, "%s\r\n", ch[i].name);
  put(buf, cap, &len, "\r\n[channel units]\r\n");
  for (int i = 0; i < numCh; ++i) put(buf, cap, &len, "%s\r\n", ch[i].unit ? ch[i].unit : "");
  if (comment && comment[0]) put(buf, cap, &len, "\r\n[comments]\r\n%s\r\n", comment);
  put(buf, cap, &len, "\r\n[column names]\r\nsats time lat long velocity heading height");
  for (int i = 0; i < numCh; ++i) {
    put(buf, cap, &len, " ");
    putColumnName(buf, cap, &len, ch[i].name);
  }
  put(buf, cap, &len, "\r\n\r\n[data]\r\n");
  return len;
}

int vboLine(char* buf, int cap, const VboSample& s, int numCh) {
  int len = 0;
  if (cap <= 0) return -1;
  buf[0] = '\0';
  const uint32_t ms = s.utcMsOfDay % 86400000u;
  put(buf, cap, &len, "%03d %02u%02u%02u.%03u ", s.sats, static_cast<unsigned>(ms / 3600000u),
      static_cast<unsigned>(ms / 60000u % 60u), static_cast<unsigned>(ms / 1000u % 60u),
      static_cast<unsigned>(ms % 1000u));
  putMinutes(buf, cap, &len, s.latE7);
  put(buf, cap, &len, " ");
  putMinutes(buf, cap, &len, -static_cast<int64_t>(s.lonE7));  // Racelogic: west positive
  const long kmh = s.speedKmh > 0.0f ? std::lround(s.speedKmh * 1000.0f) : 0;
  long hdg = std::lround(s.headingDeg * 100.0f) % 36000;
  if (hdg < 0) hdg += 36000;
  const long h = std::lround(s.heightM * 100.0f);
  const long ha = h < 0 ? -h : h;
  put(buf, cap, &len, " %03ld.%03ld %03ld.%02ld %c%05ld.%02ld", kmh / 1000, kmh % 1000, hdg / 100,
      hdg % 100, h < 0 ? '-' : '+', ha / 100, ha % 100);
  for (int i = 0; i < numCh && s.extra; ++i) {
    put(buf, cap, &len, " %+.5E", static_cast<double>(s.extra[i]));
  }
  put(buf, cap, &len, "\r\n");
  return len;
}

}  // namespace lt
