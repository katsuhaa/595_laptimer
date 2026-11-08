#include "replay.h"

#include <cstring>

#include "json.h"

namespace lt {

namespace {

// The columns of ARCH §5.2 in their order
const char* const kNames[24] = {"utc_ms", "lat", "lon", "alt_m", "speed_kmh", "heading_deg", "hacc_m", "sats",
                                "fix", "lon_g", "lat_g", "vert_g", "yaw_dps", "rpm", "throttle_pct",
                                "speed_can_kmh", "water_c", "fuel_pct", "boost_bar", "gear", "lap", "sector",
                                "lap_ms", "ghost_delta_s"};
enum Field {
  kUtc, kLat, kLon, kAlt, kSpeed, kHeading, kHacc, kSats, kFixType, kLonG, kLatG, kVertG, kYaw, kRpm,
  kThrottle, kSpeedCan, kWater, kFuel, kBoost, kGear, kLap, kSector, kLapMs, kGhost, kNumFields
};

void trim(const char** s, int* n) {
  while (*n > 0 && (**s == ' ' || **s == '\t')) {
    ++*s;
    --*n;
  }
  while (*n > 0 && ((*s)[*n - 1] == ' ' || (*s)[*n - 1] == '\t' || (*s)[*n - 1] == '\r' || (*s)[*n - 1] == '\n')) {
    --*n;
  }
}

int lineLength(const char* line, int len) {
  if (len < 0) len = static_cast<int>(std::strlen(line));
  while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) --len;
  return len;
}

bool readInt(const char* s, int n, int64_t lo, int64_t hi, int64_t* v) {
  bool exact = false;
  return json::parseScaled(s, n, 0, v, &exact) && exact && *v >= lo && *v <= hi;
}

}  // namespace

bool Replay::field(const char* line, int len, int col, const char** s, int* n) {
  len = lineLength(line, len);
  const char* p = line;
  const char* end = line + len;
  for (int c = 0;; ++c) {
    const char* comma = static_cast<const char*>(std::memchr(p, ',', static_cast<size_t>(end - p)));
    const char* fe = comma ? comma : end;
    if (c == col) {
      *s = p;
      *n = static_cast<int>(fe - p);
      return true;
    }
    if (!comma) return false;
    p = comma + 1;
  }
}

bool Replay::header(const char* line, int len) {
  numCols_ = 0;
  for (int& k : known_) k = -1;
  len = lineLength(line, len);
  if (len >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB &&
      static_cast<unsigned char>(line[2]) == 0xBF) {
    line += 3;
    len -= 3;
  }
  int cols = 0;
  const char* p = line;
  const char* end = line + len;
  for (;;) {
    if (cols >= kMaxColumns) return false;
    const char* comma = static_cast<const char*>(std::memchr(p, ',', static_cast<size_t>(end - p)));
    const char* s = p;
    int n = static_cast<int>((comma ? comma : end) - p);
    trim(&s, &n);
    at_[cols] = -1;
    for (int k = 0; k < kNumFields; ++k) {
      if (known_[k] < 0 && static_cast<int>(std::strlen(kNames[k])) == n && std::memcmp(kNames[k], s, static_cast<size_t>(n)) == 0) {
        known_[k] = cols;
        at_[cols] = static_cast<int8_t>(k);
        break;
      }
    }
    ++cols;
    if (!comma) break;
    p = comma + 1;
  }
  if (known_[kUtc] < 0 || known_[kLat] < 0 || known_[kLon] < 0 || known_[kSpeed] < 0 || known_[kFixType] < 0) {
    for (int& k : known_) k = -1;
    return false;
  }
  numCols_ = cols;
  return true;
}

int Replay::column(const char* name) const {
  if (!ready()) return -1;
  for (int k = 0; k < kNumFields; ++k) {
    if (std::strcmp(kNames[k], name) == 0) return known_[k];
  }
  return -1;
}

bool Replay::parse(const char* line, int len, ReplayRow* out, const char** why) const {
  const char* unused = nullptr;
  if (!why) why = &unused;
  *why = "";
  if (!ready()) {
    *why = "no-header";
    return false;
  }
  len = lineLength(line, len);
  if (len == 0) {
    *why = "empty";
    return false;
  }
  if (len >= 6 && std::memcmp(line, "utc_ms", 6) == 0) {
    *why = "header";
    return false;
  }
  ReplayRow r{};
  csvlog::MainRow& m = r.row;
  int64_t centiKmh = 0;
  bool haveLat = false;
  bool haveLon = false;
  int imuParts = 0;
  int cols = 0;
  const char* p = line;
  const char* end = line + len;
  for (;;) {
    const char* comma = static_cast<const char*>(std::memchr(p, ',', static_cast<size_t>(end - p)));
    const char* s = p;
    int n = static_cast<int>((comma ? comma : end) - p);
    const int k = cols < numCols_ ? at_[cols] : -1;
    trim(&s, &n);
    if (k >= 0 && n > 0) {
      bool ok = true;
      int64_t v = 0;
      switch (k) {
        case kUtc:
          ok = m.utcOk = readInt(s, n, -(1ll << 52), 1ll << 52, &m.utcMs);
          break;
        case kLat:
        case kLon:
          ok = json::parseScaled(s, n, 7, &v) && v >= -1800000000 && v <= 1800000000;
          if (ok && k == kLat) {
            m.latE7 = static_cast<int32_t>(v);
            haveLat = true;
          } else if (ok) {
            m.lonE7 = static_cast<int32_t>(v);
            haveLon = true;
          }
          break;
        case kAlt:
          ok = m.altOk = json::parseFloat(s, n, &m.altM);
          break;
        case kSpeed:
          ok = m.speedOk = json::parseScaled(s, n, 2, &centiKmh) && centiKmh > -100000000 && centiKmh < 100000000;
          if (ok) m.speedKmh = static_cast<float>(centiKmh) / 100.0f;
          break;
        case kHeading:
          ok = m.headingOk = json::parseFloat(s, n, &m.headingDeg);
          break;
        case kHacc:
          ok = m.haccOk = json::parseFloat(s, n, &m.haccM);
          break;
        case kSats:
          ok = m.satsOk = readInt(s, n, 0, 1000, &v);
          m.sats = static_cast<int>(v);
          break;
        case kFixType:
          ok = m.fixOk = readInt(s, n, 0, 10, &v);
          m.fix = static_cast<int>(v);
          break;
        case kLonG:
          ok = json::parseFloat(s, n, &m.lonG);
          imuParts += ok ? 1 : 0;
          break;
        case kLatG:
          ok = json::parseFloat(s, n, &m.latG);
          imuParts += ok ? 1 : 0;
          break;
        case kVertG:
          ok = json::parseFloat(s, n, &m.vertG);
          imuParts += ok ? 1 : 0;
          break;
        case kYaw:
          ok = m.yawOk = json::parseFloat(s, n, &m.yawDps);
          break;
        case kRpm:
          ok = m.rpmOk = readInt(s, n, -100000, 100000, &v);
          m.rpm = static_cast<int>(v);
          break;
        case kThrottle:
          ok = m.throttleOk = json::parseFloat(s, n, &m.throttlePct);
          break;
        case kSpeedCan:
          ok = m.speedCanOk = json::parseFloat(s, n, &m.speedCanKmh);
          break;
        case kWater:
          ok = m.waterOk = json::parseFloat(s, n, &m.waterC);
          break;
        case kFuel:
          ok = m.fuelOk = json::parseFloat(s, n, &m.fuelPct);
          break;
        case kBoost:
          ok = m.boostOk = json::parseFloat(s, n, &m.boostBar);
          break;
        case kGear:
          ok = m.gearOk = n == 1 && ((s[0] >= '1' && s[0] <= '5') || s[0] == 'N' || s[0] == 'R');
          if (ok) m.gear = s[0];
          break;
        case kLap:
          ok = readInt(s, n, 0, 1000000, &v);
          if (ok) m.lap = static_cast<int>(v);
          break;
        case kSector:
          ok = readInt(s, n, 0, 1000, &v);
          if (ok) m.sector = static_cast<int>(v);
          break;
        case kLapMs:
          ok = m.lapMsOk = readInt(s, n, -2147483647ll, 2147483647ll, &v);
          m.lapMs = static_cast<int32_t>(v);
          break;
        case kGhost:
          ok = m.ghostOk = json::parseFloat(s, n, &m.ghostDeltaS);
          break;
        default:
          break;
      }
      if (!ok) ++r.badFields;
    }
    ++cols;
    if (!comma) break;
    p = comma + 1;
  }
  if (cols != numCols_) {
    *why = "columns";
    return false;
  }
  m.posOk = haveLat && haveLon && m.latE7 >= -900000000 && m.latE7 <= 900000000;
  m.imuOk = imuParts == 3;
  // the same rule and values as csvlog::fixOf() of the row that was written
  r.fix = csvlog::fixFromCentiKmh(m, m.speedOk, centiKmh);
  *out = r;
  return true;
}

}  // namespace lt
