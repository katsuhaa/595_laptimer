#include "report_session.h"

#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>

#include "core/geo.h"
#include "core/ghost.h"
#include "core/vbo.h"
#include "tools/host/rcz.h"

namespace rs {

namespace {

constexpr float kSfWidthM = 50.0f;     // decided 2026-11-07: the pit lane counts, as in RaceChrono
constexpr float kSplitWidthM = 30.0f;  // sector lines
constexpr float kMinLapS = 30.0f;
constexpr double kG = 9.80665;
constexpr double kPi = 3.14159265358979323846;

int32_t e7(double deg) { return static_cast<int32_t>(std::llround(deg * 1e7)); }

bool readAll(const std::string& path, std::string* out) {
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) return false;
  out->clear();
  char buf[65536];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) out->append(buf, n);
  std::fclose(fp);
  return true;
}

std::string trim(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  const size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::string jsonStr(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"') o += "\\\"";
    else if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else if (c < 0x20 || c == '<') {
      char b[8];
      std::snprintf(b, sizeof b, "\\u%04x", c);
      o += b;
    } else {
      o += static_cast<char>(c);
    }
  }
  return o + "\"";
}

// "12.346" with `dec` decimals; "" for NaN; no "-0.000"
std::string num(double v, int dec) {
  if (std::isnan(v)) return "";
  char b[48];
  std::snprintf(b, sizeof b, "%.*f", dec, v);
  bool zero = true;
  for (const char* p = b; *p; ++p) zero = zero && (*p == '-' || *p == '0' || *p == '.');
  if (zero && b[0] == '-') return b + 1;
  return b;
}

std::string intOrEmpty(double v) { return std::isnan(v) ? "" : std::to_string(std::llround(v)); }

double wrap180(double d) {
  while (d > 180.0) d -= 360.0;
  while (d < -180.0) d += 360.0;
  return d;
}

// RaceChrono line: through A, travelling from B towards A
lt::GateDef gateOf(const vboread::Gate& g, const lt::Plane& pl, float widthM) {
  const lt::Vec2 a = pl.toLocal(e7(g.latA), e7(g.lonA));
  const lt::Vec2 b = pl.toLocal(e7(g.latB), e7(g.lonB));
  return {e7(g.latA), e7(g.lonA), lt::headingOf(a - b), widthM};
}

std::vector<lt::Fix> fixesOf(const Session& s) {
  std::vector<lt::Fix> v(s.rows.size());
  for (size_t i = 0; i < s.rows.size(); ++i) {
    const Row& r = s.rows[i];
    v[i].tUs = r.utcMs * 1000;
    v[i].valid = !std::isnan(r.lat) && !std::isnan(r.lon) && !std::isnan(r.speedKmh);
    v[i].latE7 = v[i].valid ? e7(r.lat) : 0;
    v[i].lonE7 = v[i].valid ? e7(r.lon) : 0;
    v[i].speedMps = v[i].valid ? static_cast<float>(r.speedKmh / 3.6) : 0.0f;
  }
  return v;
}

int64_t firstCrossing(const std::vector<lt::Fix>& fixes, const lt::Plane& pl, const lt::GateDef& g,
                      int64_t afterUs) {
  lt::CourseDef c{};
  c.startFinish = g;
  c.minLapS = kMinLapS;
  lt::LapTimer t;
  t.begin(pl, c);
  for (const lt::Fix& f : fixes) {
    if (f.tUs <= afterUs) continue;
    if (t.feed(f) & lt::kEvLapStart) return t.sfCrossUs();
  }
  return -1;
}

// courses.json (ARCH 5.4): the course whose S/F is within 300 m of ours gives the id and name
void courseNameFromJson(const std::string& path, double sfLat, double sfLon, Session* s) {
  std::string js;
  if (path.empty() || !readAll(path, &js)) return;
  size_t p = 0;
  while ((p = js.find("\"id\"", p)) != std::string::npos) {
    const size_t q1 = js.find('"', js.find(':', p) + 1);
    const size_t q2 = js.find('"', q1 + 1);
    const std::string id = js.substr(q1 + 1, q2 - q1 - 1);
    const size_t next = js.find("\"id\"", q2);
    const std::string obj = js.substr(p, next == std::string::npos ? std::string::npos : next - p);
    std::string name;
    const size_t n = obj.find("\"name\"");
    if (n != std::string::npos) {
      const size_t a = obj.find('"', obj.find(':', n) + 1);
      name = obj.substr(a + 1, obj.find('"', a + 1) - a - 1);
    }
    const size_t sf = obj.find("\"sf\"");
    if (sf != std::string::npos) {
      const size_t la = obj.find("\"lat\"", sf);
      const size_t lo = obj.find("\"lon\"", sf);
      if (la != std::string::npos && lo != std::string::npos) {
        const double lat = std::atof(obj.c_str() + obj.find(':', la) + 1);
        const double lon = std::atof(obj.c_str() + obj.find(':', lo) + 1);
        const double dy = (lat - sfLat) * 111000.0;
        const double dx = (lon - sfLon) * 111000.0 * std::cos(sfLat * kPi / 180.0);
        if (std::sqrt(dx * dx + dy * dy) < 300.0) {
          s->courseId = id;
          if (!name.empty()) s->courseName = name;
          return;
        }
      }
    }
    p = q2;
  }
}

// S/F and split lines from RaceChrono's definition, the splits in driving order
bool buildCourse(const vboread::File& f, Session* s, std::string* err) {
  const vboread::Gate* start = nullptr;
  std::vector<const vboread::Gate*> splits;
  for (const vboread::Gate& g : f.gates) {
    if (g.kind == "Start" && !start) start = &g;
    if (g.kind == "Split") splits.push_back(&g);
  }
  if (!start) {
    *err = f.path + ": no Start line in [laptiming]";
    return false;
  }
  s->planeLatE7 = e7(start->latA);
  s->planeLonE7 = e7(start->lonA);
  lt::Plane pl;
  pl.init(s->planeLatE7, s->planeLonE7);
  const std::vector<lt::Fix> fixes = fixesOf(*s);
  lt::CourseDef c{};
  c.startFinish = gateOf(*start, pl, kSfWidthM);
  c.minLapS = kMinLapS;
  s->lineNames = {start->name};
  const int64_t t0 = firstCrossing(fixes, pl, c.startFinish, -1);
  std::vector<std::pair<int64_t, const vboread::Gate*>> order;
  for (const vboread::Gate* g : splits) {
    const int64_t t = firstCrossing(fixes, pl, gateOf(*g, pl, kSplitWidthM), t0);
    if (t >= 0) order.push_back({t, g});
  }
  std::sort(order.begin(), order.end(),
            [](const std::pair<int64_t, const vboread::Gate*>& x,
               const std::pair<int64_t, const vboread::Gate*>& y) { return x.first < y.first; });
  for (const auto& o : order) {
    if (c.numSectors >= lt::kMaxSectors) break;
    c.sectors[c.numSectors++] = gateOf(*o.second, pl, kSplitWidthM);
    s->lineNames.push_back(o.second->name);
  }
  s->course = c;
  return true;
}

// lap, sector, lap_ms, ghost_delta_s of every row and the laps file
void timeLaps(Session* s) {
  lt::Plane pl;
  pl.init(s->planeLatE7, s->planeLonE7);
  lt::LapTimer t;
  t.begin(pl, s->course);
  const int cap = 25 * 900;  // 15 minutes at 25 Hz
  std::vector<lt::TracePoint> a(cap), b(cap);
  lt::Ghost ghost;
  ghost.begin(a.data(), b.data(), cap);
  const std::vector<lt::Fix> fixes = fixesOf(*s);
  s->laps.clear();
  for (size_t i = 0; i < fixes.size(); ++i) {
    const lt::Fix& f = fixes[i];
    Row& r = s->rows[i];
    const uint32_t ev = t.feed(f);
    if (ev & lt::kEvLap) {
      ghost.endLap(t.sfCrossUs(), t.sfCrossPos(), (ev & lt::kEvBest) != 0);
      const lt::LapResult& lr = t.lastLap();
      LapRow l;
      l.lap = lr.lapNo;
      l.startUtcMs = (lr.startUs + 500) / 1000;
      l.endUtcMs = (lr.endUs + 500) / 1000;
      l.lapMs = (lr.lapUs + 500) / 1000;
      for (int k = 0; k < lr.numSplits; ++k) l.sectorsMs.push_back((lr.splitUs[k] + 500) / 1000);
      s->laps.push_back(l);
    }
    if (ev & (lt::kEvLap | lt::kEvLapStart)) ghost.startLap(t.sfCrossUs(), t.sfCrossPos());
    r.lap = t.currentLapNo();
    r.sector = t.timing() ? t.sectorsDone() + 1 : 0;
    r.lapMs = t.timing() ? t.elapsedUs(f.tUs) / 1000 : -1;
    r.ghostDeltaS = kNan;
    if (t.timing() && f.valid) {
      const lt::Vec2 pos = pl.toLocal(f.latE7, f.lonE7);
      ghost.addSample(f.tUs, pos);
      float d = 0.0f;
      if (ghost.delta(f.tUs, pos, &d)) r.ghostDeltaS = static_cast<double>(d);
    }
  }
}

// Heading rate [deg/s, clockwise +] and acceleration along the track [m/s^2] from the GPS,
// central differences over +-k samples
void gpsDerivatives(const Session& s, int k, std::vector<double>* yaw, std::vector<double>* acc) {
  const size_t n = s.rows.size();
  yaw->assign(n, kNan);
  acc->assign(n, kNan);
  for (size_t i = static_cast<size_t>(k); i + static_cast<size_t>(k) < n; ++i) {
    const Row& a = s.rows[i - static_cast<size_t>(k)];
    const Row& b = s.rows[i + static_cast<size_t>(k)];
    const double dt = static_cast<double>(b.utcMs - a.utcMs) * 1e-3;
    if (dt <= 0.0 || dt > 1.0) continue;
    if (std::isnan(a.headingDeg) || std::isnan(b.headingDeg) || std::isnan(a.speedKmh)) continue;
    (*yaw)[i] = wrap180(b.headingDeg - a.headingDeg) / dt;
    (*acc)[i] = (b.speedKmh - a.speedKmh) / 3.6 / dt;
  }
}

double correlation(const std::vector<double>& x, const std::vector<double>& y, const std::vector<bool>& use) {
  double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
  long n = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    if (!use[i] || std::isnan(x[i]) || std::isnan(y[i])) continue;
    sx += x[i];
    sy += y[i];
    sxx += x[i] * x[i];
    syy += y[i] * y[i];
    sxy += x[i] * y[i];
    ++n;
  }
  if (n < 100) return 0.0;
  const double cov = sxy / n - sx / n * sy / n;
  const double vx = sxx / n - sx / n * sx / n;
  const double vy = syy / n - sy / n * sy / n;
  return vx > 0 && vy > 0 ? cov / std::sqrt(vx * vy) : 0.0;
}

std::string localId(int64_t utcMs, int tzMin) {
  const std::time_t t = static_cast<std::time_t>(utcMs / 1000 + tzMin * 60);
  std::tm g{};
  gmtime_r(&t, &g);
  char b[32];
  std::strftime(b, sizeof b, "%Y%m%d_%H%M%S", &g);
  return b;
}

double medianRate(const Session& s) {
  std::vector<int64_t> dt;
  for (size_t i = 1; i < s.rows.size(); ++i) dt.push_back(s.rows[i].utcMs - s.rows[i - 1].utcMs);
  if (dt.empty()) return 0.0;
  std::nth_element(dt.begin(), dt.begin() + static_cast<long>(dt.size() / 2), dt.end());
  return dt[dt.size() / 2] > 0 ? 1000.0 / static_cast<double>(dt[dt.size() / 2]) : 0.0;
}

bool mkdirs(const std::string& path) {
  std::string cur;
  std::stringstream in(path);
  std::string part;
  if (!path.empty() && path[0] == '/') cur = "/";
  while (std::getline(in, part, '/')) {
    if (part.empty()) continue;
    cur += part + "/";
    if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
  }
  return true;
}

void clearDir(const std::string& dir) {
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    const std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    ::unlink((dir + "/" + n).c_str());
  }
  closedir(d);
}

bool writeText(const std::string& path, const std::string& text, std::string* err) {
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp || std::fwrite(text.data(), 1, text.size(), fp) != text.size()) {
    if (fp) std::fclose(fp);
    *err = "cannot write " + path;
    return false;
  }
  std::fclose(fp);
  return true;
}

}  // namespace

std::string lapText(int64_t ms) {
  char b[32];
  const int64_t m = ms / 60000;
  std::snprintf(b, sizeof b, "%lld:%02lld.%03lld", static_cast<long long>(m),
                static_cast<long long>(ms / 1000 % 60), static_cast<long long>(ms % 1000));
  return b;
}

std::string mainHeader() {
  return "utc_ms,lat,lon,alt_m,speed_kmh,heading_deg,hacc_m,sats,fix,lon_g,lat_g,vert_g,yaw_dps,rpm,"
         "throttle_pct,speed_can_kmh,water_c,fuel_pct,boost_bar,gear,lap,sector,lap_ms,ghost_delta_s\n";
}

std::string mainLine(const Row& r) {
  std::string o = std::to_string(r.utcMs);
  auto f = [&o](const std::string& v) {
    o += ',';
    o += v;
  };
  f(num(r.lat, 7));
  f(num(r.lon, 7));
  f(num(r.altM, 1));
  f(num(r.speedKmh, 2));
  f(num(r.headingDeg, 1));
  f(num(r.haccM, 2));
  f(r.sats >= 0 ? std::to_string(r.sats) : "");
  f(r.fix >= 0 ? std::to_string(r.fix) : "");
  f(num(r.lonG, 3));
  f(num(r.latG, 3));
  f(num(r.vertG, 3));
  f(num(r.yawDps, 1));
  f(intOrEmpty(r.rpm));
  f(num(r.throttlePct, 1));
  f(num(r.speedCanKmh, 2));
  f(num(r.waterC, 1));
  f(num(r.fuelPct, 1));
  f(num(r.boostBar, 2));
  f(r.gear ? std::string(1, r.gear) : "");
  f(std::to_string(r.lap));
  f(std::to_string(r.sector));
  f(r.lapMs >= 0 ? std::to_string(r.lapMs) : "");
  f(num(r.ghostDeltaS, 3));
  return o + "\n";
}

std::string lapsHeader() { return "lap,start_utc_ms,end_utc_ms,lap_ms,sectors_ms\n"; }

std::string lapsLine(const LapRow& l) {
  std::string sec;
  for (size_t i = 0; i < l.sectorsMs.size(); ++i) {
    if (i) sec += ';';
    sec += std::to_string(l.sectorsMs[i]);
  }
  return std::to_string(l.lap) + "," + std::to_string(l.startUtcMs) + "," + std::to_string(l.endUtcMs) + "," +
         std::to_string(l.lapMs) + "," + sec + "\n";
}

std::string metaJson(const Session& s, const std::string& csv, const std::string& vbo, const std::string& laps) {
  int bestLap = 0;
  int64_t bestMs = 0;
  for (const LapRow& l : s.laps) {
    if (bestLap == 0 || l.lapMs < bestMs) {
      bestLap = l.lap;
      bestMs = l.lapMs;
    }
  }
  double maxKmh = 0.0, maxRpm = 0.0;
  for (const Row& r : s.rows) {
    if (!std::isnan(r.speedKmh)) maxKmh = std::max(maxKmh, r.speedKmh);
    if (!std::isnan(r.rpm)) maxRpm = std::max(maxRpm, r.rpm);
  }
  std::string o = "{\"id\":" + jsonStr(s.id) + ",\"format\":1,\"fw\":\"host\",\"tz_min\":" + std::to_string(s.tzMin);
  o += ",\"start_utc_ms\":" + std::to_string(s.rows.empty() ? 0 : s.rows.front().utcMs);
  o += ",\"end_utc_ms\":" + std::to_string(s.rows.empty() ? 0 : s.rows.back().utcMs);
  o += ",\"course\":{\"id\":" + jsonStr(s.courseId) + ",\"name\":" + jsonStr(s.courseName) + "}";
  o += ",\"laps\":" + std::to_string(s.laps.size()) + ",\"best_lap\":" + std::to_string(bestLap) +
       ",\"best_ms\":" + std::to_string(bestMs) + ",\"max_kmh\":" + num(maxKmh, 1) +
       ",\"max_rpm\":" + std::to_string(std::llround(maxRpm)) + ",\"closed\":true";
  o += ",\"files\":{\"csv\":" + jsonStr(csv) + ",\"vbo\":" + jsonStr(vbo) + ",\"laps\":" + jsonStr(laps) + "}";
  o += ",\"notes\":[";
  for (size_t i = 0; i < s.notes.size(); ++i) o += (i ? "," : "") + jsonStr(s.notes[i]);
  return o + "]}\n";
}

bool fromVboExport(const std::string& path, const Options& opt, Session* s, std::string* err) {
  *s = Session();
  s->source = path;
  s->tzMin = opt.tzMin;
  vboread::File vf;
  if (!vboread::read(path, &vf, err)) return false;
  s->courseName = vf.sessionName;
  std::string all;
  if (!readAll(path, &all)) {
    *err = "cannot read " + path;
    return false;
  }
  // Date of the first line: "File created on 31/10/2026 at 04:50:23" (UTC); "@" in our own files
  int dd = 0, mo = 0, yy = 0;
  if (std::sscanf(vf.created.c_str(), "File created on %d/%d/%d", &dd, &mo, &yy) != 3) {
    *err = path + ": no date in the first line";
    return false;
  }
  std::tm day{};
  day.tm_year = yy - 1900;
  day.tm_mon = mo - 1;
  day.tm_mday = dd;
  const int64_t dayMs = static_cast<int64_t>(timegm(&day)) * 1000;

  std::istringstream in(all);
  std::string line, section;
  std::vector<std::string> cols;
  std::vector<std::vector<double>> v;  // [column][row]
  std::vector<int64_t> tms;
  int64_t dayOffset = 0;
  int64_t prevTod = -1;
  bool data = false;
  while (std::getline(in, line)) {
    line = trim(line);
    if (!data) {
      if (line.empty()) continue;
      if (line[0] == '[') {
        section = line;
        data = section == "[data]";
        continue;
      }
      if (section == "[column names]" && cols.empty()) {
        std::istringstream c(line);
        std::string name;
        while (c >> name) cols.push_back(name);
      }
      continue;
    }
    if (line.empty()) continue;
    if (cols.empty()) {
      *err = path + ": no [column names]";
      return false;
    }
    if (v.empty()) v.assign(cols.size(), std::vector<double>());
    std::istringstream d(line);
    std::string tok;
    size_t k = 0;
    int64_t tod = -1;
    std::vector<double> rowv(cols.size(), kNan);
    while (d >> tok && k < cols.size()) {
      if (cols[k] == "time" && tok.size() >= 6) {
        const int hh = (tok[0] - '0') * 10 + (tok[1] - '0');
        const int mm = (tok[2] - '0') * 10 + (tok[3] - '0');
        const double ss = std::atof(tok.c_str() + 4);
        tod = (hh * 3600 + mm * 60) * 1000LL + std::llround(ss * 1000.0);
      } else {
        rowv[k] = std::atof(tok.c_str());
      }
      ++k;
    }
    if (tod < 0) continue;
    if (prevTod >= 0 && tod + 12LL * 3600 * 1000 < prevTod) dayOffset += 86400000LL;  // past midnight UTC
    prevTod = tod;
    tms.push_back(dayMs + dayOffset + tod);
    for (size_t c = 0; c < cols.size(); ++c) v[c].push_back(rowv[c]);
  }
  auto col = [&](const char* name) -> const std::vector<double>* {
    for (size_t i = 0; i < cols.size() && i < v.size(); ++i) {
      if (cols[i] == name) return &v[i];
    }
    return nullptr;
  };
  const std::vector<double>* lat = col("lat");
  const std::vector<double>* lon = col("long");
  const std::vector<double>* spd = col("velocity");
  if (tms.empty() || !lat || !lon || !spd) {
    *err = path + ": no data rows with lat/long/velocity";
    return false;
  }
  const std::vector<double>* sats = col("sats");
  const std::vector<double>* hdg = col("heading");
  const std::vector<double>* hgt = col("height");
  const std::vector<double>* prec = col("coordinate_precision");
  const std::vector<double>* rpm = col("rpm-canbus");
  const std::vector<double>* thr = col("accelerator_pos-canbus");
  const std::vector<double>* vcan = col("velocity-canbus");
  const std::vector<double>* water = col("coolant_temp-canbus");
  const std::vector<double>* fuel = col("fuel_level-canbus");
  const std::vector<double>* boost = col("manifold_relative_pressure-canbus");
  const std::vector<double>* latc = col("latacc-canbus");
  const std::vector<double>* lonc = col("longacc-canbus");
  const std::vector<double>* zc = col("z_acc-canbus");
  const std::vector<double>* yawc = col("yaw_rate-canbus");
  const std::vector<double>* latcalc = col("latacc-calc");
  const std::vector<double>* loncalc = col("longacc-calc");

  const size_t n = tms.size();
  s->rows.resize(n);
  for (size_t i = 0; i < n; ++i) {
    Row& r = s->rows[i];
    r.utcMs = tms[i];
    r.lat = (*lat)[i] / 60.0;
    r.lon = -(*lon)[i] / 60.0;  // Racelogic: minutes, west positive
    r.speedKmh = (*spd)[i];
    r.headingDeg = hdg ? (*hdg)[i] : kNan;
    r.altM = hgt ? (*hgt)[i] : kNan;
    r.haccM = prec ? (*prec)[i] : kNan;
    r.sats = sats ? static_cast<int>((*sats)[i]) : -1;
    r.fix = r.sats < 0 || r.sats >= 4 ? 3 : 0;
    r.rpm = rpm ? (*rpm)[i] : kNan;
    r.throttlePct = thr ? (*thr)[i] : kNan;
    r.speedCanKmh = vcan ? (*vcan)[i] : kNan;
    r.waterC = water ? (*water)[i] : kNan;
    r.fuelPct = fuel ? (*fuel)[i] : kNan;
    r.boostBar = boost ? (*boost)[i] * 0.05 : kNan;
    r.vertG = zc ? (*zc)[i] : kNan;
  }
  // RaceChrono writes 0 for a CAN channel until its first frame. Where 0 cannot be a real value
  // (rpm with the engine running, water temperature, fuel) those rows are empty (ARCH 5.2)
  int blank = 0;
  for (double Row::*m : {&Row::rpm, &Row::waterC, &Row::fuelPct}) {
    size_t i = 0;
    while (i < n && !std::isnan(s->rows[i].*m) && s->rows[i].*m == 0.0) ++i;
    if (i == n) i = 0;  // all zero: a real value (e.g. throttle never pressed) - keep
    for (size_t k = 0; k < i; ++k) s->rows[k].*m = kNan;
    blank = std::max(blank, static_cast<int>(i));
  }
  s->rateHz = medianRate(*s);
  s->id = localId(s->rows.front().utcMs, s->tzMin);
  s->notes.push_back("RaceChrono の VBO 書き出し（" + std::to_string(std::lround(s->rateHz)) +
                     " Hz）から作ったセッション。ラップは本機と同じ計算（core/lap、RaceChrono のコース定義）");
  if (boost) {
    s->notes.push_back("ブースト: RaceChrono の manifold_relative_pressure-canbus × 0.05 bar（換算は未確認・五分）");
  }
  if (blank > 0) {
    s->notes.push_back("回転数・水温・燃料: 最初の CAN フレームより前に RaceChrono が書いた 0 は空欄にした（最長 " +
                       std::to_string(blank) + " 行）");
  }

  // G and yaw from the car (CAN): signs set to ours (forward +, right +, left turn +) by comparing
  // with the GPS track
  std::vector<double> yawGps, accGps;
  gpsDerivatives(*s, 1, &yawGps, &accGps);
  std::vector<double> latGps(n, kNan), lonGps(n, kNan), yawLeft(n, kNan);
  std::vector<bool> moving(n, false);
  for (size_t i = 0; i < n; ++i) {
    const double vms = s->rows[i].speedKmh / 3.6;
    moving[i] = vms > 8.0;
    if (!std::isnan(yawGps[i])) {
      latGps[i] = vms * yawGps[i] * kPi / 180.0 / kG;
      yawLeft[i] = -yawGps[i];
    }
    if (!std::isnan(accGps[i])) lonGps[i] = accGps[i] / kG;
  }
  const std::vector<double>* latSrc = latc ? latc : latcalc;
  const std::vector<double>* lonSrc = lonc ? lonc : loncalc;
  const double cLat = latSrc ? correlation(*latSrc, latGps, moving) : 0.0;
  const double cLon = lonSrc ? correlation(*lonSrc, lonGps, moving) : 0.0;
  const double cYaw = yawc ? correlation(*yawc, yawLeft, moving) : 0.0;
  char note[200];
  std::snprintf(note, sizeof note, "G・ヨー: RaceChrono の %s（GPS との相関 横 %+.2f、前後 %+.2f、ヨー %+.2f。負は符号を反転）",
                latc ? "-canbus" : "-calc", cLat, cLon, cYaw);
  s->notes.push_back(note);
  for (size_t i = 0; i < n; ++i) {
    Row& r = s->rows[i];
    if (latSrc && std::fabs(cLat) >= 0.5) r.latG = (*latSrc)[i] * (cLat < 0 ? -1.0 : 1.0);
    if (lonSrc && std::fabs(cLon) >= 0.5) r.lonG = (*lonSrc)[i] * (cLon < 0 ? -1.0 : 1.0);
    if (yawc && std::fabs(cYaw) >= 0.5) r.yawDps = (*yawc)[i] * (cYaw < 0 ? -1.0 : 1.0);
  }

  if (!buildCourse(vf, s, err)) return false;
  courseNameFromJson(opt.coursesJson, s->planeLatE7 * 1e-7, s->planeLonE7 * 1e-7, s);
  timeLaps(s);
  return true;
}

bool fromRcz(const std::string& dir, const Options& opt, Session* s, std::string* err) {
  *s = Session();
  s->source = dir;
  s->tzMin = opt.tzMin;
  rcz::Session z;
  if (!rcz::load(dir, &z, err)) return false;
  if (!opt.course) {
    *err = "a RaceChrono VBO export with the track definition is needed for " + dir;
    return false;
  }
  s->courseName = z.trackName;
  const size_t n = z.tMs.size();
  s->rows.resize(n);
  for (size_t i = 0; i < n; ++i) {
    Row& r = s->rows[i];
    r.utcMs = z.tMs[i];
    r.lat = z.latE7[i] * 1e-7;
    r.lon = z.lonE7[i] * 1e-7;
    r.speedKmh = z.speedMps[i] * 3.6;
    r.headingDeg = z.bearingDeg[i];
    r.altM = z.altM[i];
    r.sats = z.sats.empty() ? -1 : z.sats[i];
    r.fix = r.sats < 0 || r.sats >= 4 ? 3 : 0;
  }
  s->rateHz = medianRate(*s);
  s->id = localId(s->rows.front().utcMs, s->tzMin);
  // G and yaw from the GPS track, over about +-0.1 s
  const int k = std::max(1, static_cast<int>(std::lround(s->rateHz * 0.1)));
  std::vector<double> yaw, acc;
  gpsDerivatives(*s, k, &yaw, &acc);
  for (size_t i = 0; i < n; ++i) {
    Row& r = s->rows[i];
    if (std::isnan(yaw[i])) continue;
    r.yawDps = -yaw[i];
    r.latG = r.speedKmh / 3.6 * yaw[i] * kPi / 180.0 / kG;
    r.lonG = acc[i] / kG;
  }
  for (const rcz::Lap& l : z.laps) {
    if (l.finished) s->raceChronoLaps.push_back({l.startMs, l.finishMs});
  }
  s->notes.push_back("RaceChrono のセッション（.rcz、GPS " + std::to_string(std::lround(s->rateHz)) +
                     " Hz）から作ったセッション。CAN の値なし。G とヨーは GPS から計算");
  if (!buildCourse(*opt.course, s, err)) return false;
  courseNameFromJson(opt.coursesJson, s->planeLatE7 * 1e-7, s->planeLonE7 * 1e-7, s);
  timeLaps(s);
  return true;
}

std::vector<lt::LapResult> lapTimer(const Session& s, const lt::CourseDef& c) {
  lt::Plane pl;
  pl.init(s.planeLatE7, s.planeLonE7);
  lt::LapTimer t;
  t.begin(pl, c);
  std::vector<lt::LapResult> laps;
  for (const lt::Fix& f : fixesOf(s)) {
    if (t.feed(f) & lt::kEvLap) laps.push_back(t.lastLap());
  }
  return laps;
}

bool writeFolder(const Session& s, const std::string& root, std::string* dirOut, std::string* err) {
  const std::string dir = root + "/sessions/" + s.id;
  if (!mkdirs(dir)) {
    *err = "cannot create " + dir;
    return false;
  }
  clearDir(dir);
  const std::string base = s.id + "_" + s.courseId;
  const std::string csv = base + ".csv", laps = base + "_laps.csv", vbo = base + ".vbo";
  std::string text = mainHeader();
  for (const Row& r : s.rows) text += mainLine(r);
  if (!writeText(dir + "/" + csv, text, err)) return false;
  text = lapsHeader();
  for (const LapRow& l : s.laps) text += lapsLine(l);
  if (!writeText(dir + "/" + laps, text, err)) return false;

  // VBO as the device writes it (core/vbo), with the CAN channels the session has
  const lt::VboChannel ch[4] = {{"rpm-canbus", ""},
                                {"accelerator_pos-canbus", ""},
                                {"coolant_temp-canbus", ""},
                                {"fuel_level-canbus", ""}};
  const std::time_t t0 = static_cast<std::time_t>(s.rows.front().utcMs / 1000);
  std::tm g{};
  gmtime_r(&t0, &g);
  const lt::VboDateTime created{g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec};
  std::vector<char> buf(4096);
  const std::string comment = "595_laptimer host report: " + s.id;
  int n = lt::vboHeader(buf.data(), static_cast<int>(buf.size()), created, ch, 4, comment.c_str());
  text.assign(buf.data(), static_cast<size_t>(n));
  for (const Row& r : s.rows) {
    if (std::isnan(r.lat)) continue;
    const float extra[4] = {static_cast<float>(std::isnan(r.rpm) ? 0.0 : r.rpm),
                            static_cast<float>(std::isnan(r.throttlePct) ? 0.0 : r.throttlePct),
                            static_cast<float>(std::isnan(r.waterC) ? 0.0 : r.waterC),
                            static_cast<float>(std::isnan(r.fuelPct) ? 0.0 : r.fuelPct)};
    lt::VboSample v{};
    v.sats = r.sats < 0 ? 0 : r.sats;
    v.utcMsOfDay = static_cast<uint32_t>(r.utcMs % 86400000);
    v.latE7 = e7(r.lat);
    v.lonE7 = e7(r.lon);
    v.speedKmh = static_cast<float>(r.speedKmh);
    v.headingDeg = static_cast<float>(std::isnan(r.headingDeg) ? 0.0 : r.headingDeg);
    v.heightM = static_cast<float>(std::isnan(r.altM) ? 0.0 : r.altM);
    v.extra = extra;
    n = lt::vboLine(buf.data(), static_cast<int>(buf.size()), v, 4);
    text.append(buf.data(), static_cast<size_t>(n));
  }
  if (!writeText(dir + "/" + vbo, text, err)) return false;
  if (!writeText(dir + "/meta.json", metaJson(s, csv, vbo, laps), err)) return false;
  *dirOut = dir;
  return true;
}

}  // namespace rs
