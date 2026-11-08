#include "csvlog.h"

#include <cmath>
#include <cstring>

#include "json.h"
#include "maint.h"

namespace lt {
namespace csvlog {

const char kMainHeader[] =
    "utc_ms,lat,lon,alt_m,speed_kmh,heading_deg,hacc_m,sats,fix,lon_g,lat_g,vert_g,yaw_dps,rpm,"
    "throttle_pct,speed_can_kmh,water_c,fuel_pct,boost_bar,gear,lap,sector,lap_ms,ghost_delta_s\n";
const char kLapsHeader[] = "lap,start_utc_ms,end_utc_ms,lap_ms,sectors_ms\n";
const char kImuHeader[] = "t_ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps\n";
const char kCanHeader[] = "t_ms,bus,id,dlc,data\n";

namespace {

// Text appended into the caller's buffer; on overflow everything fails (finish() = 0)
class Out {
 public:
  Out(char* buf, int cap) : buf_(buf), cap_(cap) {
    if (cap_ > 0) {
      buf_[0] = '\0';
    } else {
      ok_ = false;
    }
  }
  void put(const char* s, int n) {
    if (!ok_ || n < 0) return;
    if (len_ + n >= cap_) {
      ok_ = false;
      return;
    }
    std::memcpy(buf_ + len_, s, static_cast<size_t>(n));
    len_ += n;
  }
  void str(const char* s) { put(s, static_cast<int>(std::strlen(s))); }
  void ch(char c) { put(&c, 1); }
  void i64(int64_t v) { scaled(v, 0); }
  void scaled(int64_t v, int decimals) {
    char t[32];
    const int n = json::formatScaled(t, sizeof t, v, decimals);
    if (n <= 0) {
      ok_ = false;
      return;
    }
    put(t, n);
  }
  // A float with `decimals`, or nothing when it is not valid
  void fixed(bool valid, float v, int decimals) {
    int64_t q = 0;
    if (!valid || !json::roundScaled(v, decimals, &q)) return;
    scaled(q, decimals);
  }
  void hex(uint32_t v, int digits) {
    static const char kHex[] = "0123456789ABCDEF";
    char t[8];
    for (int i = digits - 1; i >= 0; --i) {
      t[i] = kHex[v & 15u];
      v >>= 4;
    }
    put(t, digits);
  }
  int finish() {
    if (!ok_) {
      if (cap_ > 0) buf_[0] = '\0';
      return 0;
    }
    buf_[len_] = '\0';
    return len_;
  }

 private:
  char* buf_;
  int cap_;
  int len_ = 0;
  bool ok_ = true;
};

int64_t usToMs(int64_t us) { return us >= 0 ? (us + 500) / 1000 : -((-us + 500) / 1000); }

bool gearValid(char g) { return (g >= '1' && g <= '5') || g == 'N' || g == 'R'; }

int copyText(char* buf, int cap, const char* text) {
  Out o(buf, cap);
  o.str(text);
  return o.finish();
}

}  // namespace

int mainHeader(char* buf, int cap) { return copyText(buf, cap, kMainHeader); }

int mainLine(char* buf, int cap, const MainRow& r) {
  Out o(buf, cap);
  if (r.utcOk) o.i64(r.utcMs);
  o.ch(',');
  if (r.posOk) o.scaled(r.latE7, 7);
  o.ch(',');
  if (r.posOk) o.scaled(r.lonE7, 7);
  o.ch(',');
  o.fixed(r.altOk, r.altM, 1);
  o.ch(',');
  o.fixed(r.speedOk, r.speedKmh, 2);
  o.ch(',');
  o.fixed(r.headingOk, r.headingDeg, 1);
  o.ch(',');
  o.fixed(r.haccOk, r.haccM, 2);
  o.ch(',');
  if (r.satsOk) o.i64(r.sats);
  o.ch(',');
  if (r.fixOk) o.i64(r.fix);
  o.ch(',');
  o.fixed(r.imuOk, r.lonG, 3);
  o.ch(',');
  o.fixed(r.imuOk, r.latG, 3);
  o.ch(',');
  o.fixed(r.imuOk, r.vertG, 3);
  o.ch(',');
  o.fixed(r.yawOk, r.yawDps, 1);
  o.ch(',');
  if (r.rpmOk) o.i64(r.rpm);
  o.ch(',');
  o.fixed(r.throttleOk, r.throttlePct, 1);
  o.ch(',');
  o.fixed(r.speedCanOk, r.speedCanKmh, 2);
  o.ch(',');
  o.fixed(r.waterOk, r.waterC, 1);
  o.ch(',');
  o.fixed(r.fuelOk, r.fuelPct, 1);
  o.ch(',');
  o.fixed(r.boostOk, r.boostBar, 2);
  o.ch(',');
  if (r.gearOk && gearValid(r.gear)) o.ch(r.gear);
  o.ch(',');
  o.i64(r.lap);
  o.ch(',');
  o.i64(r.sector);
  o.ch(',');
  if (r.lapMsOk) o.i64(r.lapMs);
  o.ch(',');
  o.fixed(r.ghostOk, r.ghostDeltaS, 3);
  o.ch('\n');
  return o.finish();
}

Fix fixFromCentiKmh(const MainRow& r, bool speedOk, int64_t centiKmh) {
  Fix f{};
  f.tUs = r.utcMs * 1000;
  f.latE7 = r.latE7;
  f.lonE7 = r.lonE7;
  f.speedMps = speedOk ? static_cast<float>(centiKmh) / 360.0f : 0.0f;
  f.valid = r.utcOk && r.posOk && speedOk && r.fixOk && r.fix == 3;
  return f;
}

Fix fixOf(const MainRow& r) {
  int64_t centiKmh = 0;
  const bool speed = r.speedOk && json::roundScaled(r.speedKmh, 2, &centiKmh);
  return fixFromCentiKmh(r, speed, centiKmh);
}

void setLapFields(MainRow* r, const LapTimer& t, int64_t tUs) {
  r->lap = t.currentLapNo();
  r->sector = t.timing() ? t.sectorsDone() + 1 : 0;
  r->lapMsOk = t.timing();
  const int64_t ms = t.timing() ? (tUs - t.lapStartUs()) / 1000 : 0;
  r->lapMs = static_cast<int32_t>(ms > 2147483647 ? 2147483647 : ms < 0 ? 0 : ms);
}

int lapsHeader(char* buf, int cap) { return copyText(buf, cap, kLapsHeader); }

int lapsLine(char* buf, int cap, int lap, int64_t startUtcMs, int64_t endUtcMs, int64_t lapMs,
             const int64_t* sectorMs, int numSectors) {
  Out o(buf, cap);
  o.i64(lap);
  o.ch(',');
  o.i64(startUtcMs);
  o.ch(',');
  o.i64(endUtcMs);
  o.ch(',');
  o.i64(lapMs);
  o.ch(',');
  for (int i = 0; i < numSectors && sectorMs; ++i) {
    if (i > 0) o.ch(';');
    o.i64(sectorMs[i]);
  }
  o.ch('\n');
  return o.finish();
}

int lapsLine(char* buf, int cap, const LapResult& r) {
  int64_t sec[kMaxSectors + 1];
  const int n = r.numSplits > 0 && r.numSplits <= kMaxSectors + 1 ? r.numSplits : 0;
  for (int i = 0; i < n; ++i) sec[i] = usToMs(r.splitUs[i]);
  return lapsLine(buf, cap, r.lapNo, usToMs(r.startUs), usToMs(r.endUs), usToMs(r.lapUs), sec, n);
}

int imuHeader(char* buf, int cap) { return copyText(buf, cap, kImuHeader); }

int imuLine(char* buf, int cap, const ImuRow& r) {
  Out o(buf, cap);
  o.i64(r.tMs);
  const float v[6] = {r.ax, r.ay, r.az, r.gx, r.gy, r.gz};
  for (int i = 0; i < 6; ++i) {
    o.ch(',');
    o.fixed(true, v[i], i < 3 ? 4 : 2);
  }
  o.ch('\n');
  return o.finish();
}

int canHeader(char* buf, int cap) { return copyText(buf, cap, kCanHeader); }

int canLine(char* buf, int cap, int64_t tMs, char bus, uint32_t id, int dlc, const uint8_t* data) {
  Out o(buf, cap);
  o.i64(tMs);
  o.ch(',');
  o.ch(bus);
  o.ch(',');
  o.hex(id, 8);
  o.ch(',');
  o.i64(dlc);
  o.ch(',');
  const int n = dlc < 0 ? 0 : dlc > 8 ? 8 : dlc;
  for (int i = 0; i < n && data; ++i) o.hex(data[i], 2);
  o.ch('\n');
  return o.finish();
}

int metaJson(char* buf, int cap, const Meta& m) {
  const bool course = m.courseId && m.courseId[0];
  const char* cid = course ? m.courseId : "street";
  json::Writer w(buf, cap);
  w.beginObject();
  w.key("id").str(m.id).key("format").i64(1).key("fw").str(m.fw).key("tz_min").i64(m.tzMin);
  w.key("start_utc_ms").i64(m.startUtcMs).key("end_utc_ms").i64(m.endUtcMs);
  w.key("course");
  if (course) {
    w.beginObject(true).key("id").str(m.courseId).key("name").str(m.courseName ? m.courseName : "").endObject();
  } else {
    w.null();
  }
  w.key("laps").i64(m.laps);
  if (m.bestLap > 0) {
    w.key("best_lap").i64(m.bestLap).key("best_ms").i64(m.bestMs);
  } else {
    w.key("best_lap").null().key("best_ms").null();
  }
  w.key("max_kmh");
  if (m.maxKmhOk) {
    w.num(m.maxKmh, 1, false);
  } else {
    w.null();
  }
  w.key("max_rpm");
  if (m.maxRpmOk) {
    w.i64(m.maxRpm);
  } else {
    w.null();
  }
  w.key("closed").boolean(m.closed);
  static const char* const kKeys[6] = {"csv", "vbo", "ubx", "imu", "can", "laps"};
  static const FileKind kKinds[6] = {FileKind::Csv, FileKind::Vbo, FileKind::Ubx,
                                     FileKind::Imu, FileKind::Can, FileKind::Laps};
  w.key("files").beginObject(true);
  for (int i = 0; i < 6; ++i) {
    char name[96];
    if (fileName(name, sizeof name, m.id, cid, kKinds[i]) == 0) return 0;
    w.key(kKeys[i]).str(name);
  }
  w.endObject().endObject();
  return w.finish();
}

int sessionId(char* buf, int cap, int64_t utcMs, int tzMin) {
  const int64_t local = utcMs + static_cast<int64_t>(tzMin) * 60000;
  const int32_t day = dayOfUtcMs(utcMs, tzMin);
  const int64_t msOfDay = local - static_cast<int64_t>(day) * 86400000;
  const int sOfDay = static_cast<int>(msOfDay / 1000);
  int y = 0, mo = 0, d = 0;
  civilFromDays(day, &y, &mo, &d);
  if (y < 0 || y > 9999) return 0;
  const int v[6] = {y, mo, d, sOfDay / 3600, sOfDay / 60 % 60, sOfDay % 60};
  char t[16];
  int k = 0;
  for (int i = 0; i < 6; ++i) {
    const int width = i == 0 ? 4 : 2;
    int x = v[i];
    for (int j = width - 1; j >= 0; --j) {
      t[k + j] = static_cast<char>('0' + x % 10);
      x /= 10;
    }
    k += width;
    if (i == 2) t[k++] = '_';
  }
  t[k] = '\0';
  return copyText(buf, cap, t);
}

int fileName(char* buf, int cap, const char* id, const char* cid, FileKind kind) {
  static const char* const kSuffix[6] = {".csv", ".vbo", "_raw.ubx", "_imu.csv", "_can.csv", "_laps.csv"};
  Out o(buf, cap);
  o.str(id ? id : "");
  o.ch('_');
  o.str(cid && cid[0] ? cid : "street");
  o.str(kSuffix[static_cast<int>(kind)]);
  return o.finish();
}

}  // namespace csvlog
}  // namespace lt
