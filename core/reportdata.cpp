#include "reportdata.h"

#include <cstring>

namespace lt {

namespace report {

namespace {

const char* const kMain[kNumMainColumns] = {
    "utc_ms",   "lat",     "lon",     "alt_m",        "speed_kmh",     "heading_deg",
    "hacc_m",   "sats",    "fix",     "lon_g",        "lat_g",         "vert_g",
    "yaw_dps",  "rpm",     "throttle_pct", "speed_can_kmh", "water_c", "fuel_pct",
    "boost_bar", "gear",   "lap",     "sector",       "lap_ms",        "ghost_delta_s"};

// What report.html needs: time, position, speed, G, engine, laps
const char* const kReport[kNumColumns] = {
    "utc_ms", "lat",          "lon",       "speed_kmh", "lon_g", "lat_g",  "rpm",
    "throttle_pct", "boost_bar", "water_c", "fuel_pct",  "lap",   "sector", "lap_ms"};

bool safeChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' ||
         c == '+' || c == '-';
}

bool same(const char* a, int n, const char* b) {
  return static_cast<int>(std::strlen(b)) == n && std::memcmp(a, b, static_cast<size_t>(n)) == 0;
}

// Integer milliseconds: optional '-', 1..19 digits
bool parseInt64(const char* s, int n, int64_t* v) {
  int i = 0;
  bool neg = false;
  if (n > 0 && s[0] == '-') {
    neg = true;
    i = 1;
  }
  if (i >= n || n - i > 18) return false;
  int64_t r = 0;
  for (; i < n; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    r = r * 10 + (s[i] - '0');
  }
  *v = neg ? -r : r;
  return true;
}

int headerLength() {
  int n = 0;
  for (int i = 0; i < kNumColumns; ++i) n += static_cast<int>(std::strlen(kReport[i])) + 1;
  return n;
}

}  // namespace

const char* mainColumn(int i) { return i >= 0 && i < kNumMainColumns ? kMain[i] : ""; }
const char* column(int i) { return i >= 0 && i < kNumColumns ? kReport[i] : ""; }

bool Decimator::keep(int64_t utcMs) {
  if (have_ && utcMs >= last_ - kBackJumpMs && utcMs < last_ + periodMs_) return false;
  have_ = true;
  last_ = utcMs;
  return true;
}

void LogFilter::begin(char* lineBuf, int lineCap, int64_t periodMs) {
  buf_ = lineBuf;
  lineCap_ = lineCap;
  len_ = 0;
  overflow_ = false;
  seenFirst_ = false;
  utcCol_ = -1;
  for (int i = 0; i < kNumColumns; ++i) map_[i] = -1;
  dec_ = Decimator(periodMs);
  st_ = FilterStats();
}

int LogFilter::feed(const char* in, int n, char* out, int cap, int* produced) {
  int used = 0;
  int i = 0;
  while (i < n) {
    const char c = in[i];
    if (c == '\n') {
      int k = 0;
      if (!line(out + used, cap - used, &k)) break;  // the LF stays: this line comes again
      used += k;
      len_ = 0;
      overflow_ = false;
      ++i;
      continue;
    }
    if (!overflow_) {
      if (len_ < lineCap_) buf_[len_++] = c;
      else overflow_ = true;  // dropped at its LF
    }
    ++i;
  }
  *produced = used;
  return i;
}

void LogFilter::finish() {
  if (len_ > 0 || overflow_) {
    if (seenFirst_) ++st_.rowsIn;
    ++st_.rowsBad;  // no LF: the line was cut (power off while writing)
  }
  len_ = 0;
  overflow_ = false;
}

// Splits buf_[0..len) at commas into fs_/fe_
static int splitFields(const char* b, int len, int* fs, int* fe, int maxFields) {
  int nf = 0;
  int start = 0;
  for (int i = 0; i <= len && nf < maxFields; ++i) {
    if (i == len || b[i] == ',') {
      fs[nf] = start;
      fe[nf] = i;
      ++nf;
      start = i + 1;
    }
  }
  return nf;
}

bool LogFilter::header(int len) {
  numFields_ = splitFields(buf_, len, fs_, fe_, kMaxFields);
  int utc = -1;
  for (int f = 0; f < numFields_; ++f) {
    if (same(buf_ + fs_[f], fe_[f] - fs_[f], "utc_ms")) utc = f;
  }
  if (utc < 0) return false;
  utcCol_ = utc;
  st_.missingColumns = 0;
  for (int c = 0; c < kNumColumns; ++c) {
    map_[c] = -1;
    for (int f = 0; f < numFields_; ++f) {
      if (same(buf_ + fs_[f], fe_[f] - fs_[f], kReport[c])) {
        map_[c] = f;
        break;
      }
    }
    if (map_[c] < 0) ++st_.missingColumns;
  }
  return true;
}

// The kept row's report fields into out (room checked by the caller)
int LogFilter::pick(int len, char* out) {
  const int nf = splitFields(buf_, len, fs_, fe_, kMaxFields);
  int o = 0;
  for (int c = 0; c < kNumColumns; ++c) {
    if (c) out[o++] = ',';
    const int f = map_[c];
    if (f < 0 || f >= nf) continue;
    const int s = fs_[f];
    const int e = fe_[f];
    bool ok = true;
    for (int i = s; i < e; ++i) ok = ok && safeChar(buf_[i]);
    if (!ok) {
      ++st_.badFields;
      continue;
    }
    std::memcpy(out + o, buf_ + s, static_cast<size_t>(e - s));
    o += e - s;
  }
  out[o++] = '\n';
  return o;
}

bool LogFilter::line(char* out, int cap, int* produced) {
  *produced = 0;
  int len = len_;
  if (len > 0 && buf_[len - 1] == '\r') --len;
  if (!seenFirst_) {
    if (overflow_ || len == 0) return true;  // nothing usable yet
    const int need = headerLength() + 1;
    if (cap < need) return false;
    int o = 0;
    for (int c = 0; c < kNumColumns; ++c) {
      const int n = static_cast<int>(std::strlen(kReport[c]));
      if (c) out[o++] = ',';
      std::memcpy(out + o, kReport[c], static_cast<size_t>(n));
      o += n;
    }
    out[o++] = '\n';
    *produced = o;
    seenFirst_ = true;
    if (header(len)) {
      st_.header = true;
      return true;
    }
    // No header (damaged file): assume the ARCH 5.2 order and use this line as data
    for (int c = 0; c < kNumColumns; ++c) {
      map_[c] = -1;
      for (int m = 0; m < kNumMainColumns; ++m) {
        if (std::strcmp(kMain[m], kReport[c]) == 0) map_[c] = m;
      }
    }
    utcCol_ = 0;
    int k = 0;
    if (!line(out + o, cap - o, &k)) {
      // keep it simple: the header is out, the first data row is dropped
      ++st_.rowsIn;
      ++st_.rowsBad;
      return true;
    }
    *produced = o + k;
    return true;
  }
  if (len == 0 && !overflow_) return true;  // empty line
  ++st_.rowsIn;
  if (overflow_) {
    ++st_.rowsBad;
    return true;
  }
  const int nf = splitFields(buf_, len, fs_, fe_, kMaxFields);
  int64_t t = 0;
  if (utcCol_ >= nf || !parseInt64(buf_ + fs_[utcCol_], fe_[utcCol_] - fs_[utcCol_], &t)) {
    ++st_.rowsBad;
    return true;
  }
  if (cap < len + kNumColumns + 1) {
    --st_.rowsIn;  // counted again when the line comes back
    return false;
  }
  if (!dec_.keep(t)) return true;
  *produced = pick(len, out);
  if (st_.rowsOut == 0) st_.firstUtcMs = t;
  st_.lastUtcMs = t;
  ++st_.rowsOut;
  return true;
}

int escapeForScript(const char* in, int n, char* out, int cap, int* consumed) {
  int o = 0;
  int i = 0;
  for (; i < n; ++i) {
    if (in[i] == '<') {
      if (o + 6 > cap) break;
      std::memcpy(out + o, "\\u003c", 6);
      o += 6;
    } else {
      if (o + 1 > cap) break;
      out[o++] = in[i];
    }
  }
  *consumed = i;
  return o;
}

int formatInfo(char* out, int cap, const Info& info) {
  using webfmt::put;
  using webfmt::putInt;
  int len = 0;
  if (cap <= 0) return -1;
  put(out, cap, &len, "{\"generator\":");
  webfmt::putJsonString(out, cap, &len, info.generator ? info.generator : "");
  put(out, cap, &len, ",\"period_ms\":");
  putInt(out, cap, &len, info.periodMs);
  if (info.log) {
    const FilterStats& s = *info.log;
    put(out, cap, &len, ",\"rows_in\":");
    putInt(out, cap, &len, s.rowsIn);
    put(out, cap, &len, ",\"rows_out\":");
    putInt(out, cap, &len, s.rowsOut);
    put(out, cap, &len, ",\"rows_bad\":");
    putInt(out, cap, &len, s.rowsBad);
    put(out, cap, &len, ",\"first_utc_ms\":");
    putInt(out, cap, &len, s.firstUtcMs);
    put(out, cap, &len, ",\"last_utc_ms\":");
    putInt(out, cap, &len, s.lastUtcMs);
    put(out, cap, &len, s.header ? ",\"header\":true" : ",\"header\":false");
    put(out, cap, &len, ",\"missing_columns\":");
    putInt(out, cap, &len, s.missingColumns);
    put(out, cap, &len, ",\"bad_fields\":");
    putInt(out, cap, &len, s.badFields);
  }
  put(out, cap, &len, ",\"notes\":[");
  for (int i = 0; i < info.numNotes; ++i) {
    if (i) put(out, cap, &len, ",");
    webfmt::putJsonString(out, cap, &len, info.notes[i] ? info.notes[i] : "");
  }
  put(out, cap, &len, "]}");
  if (len >= 0 && len < cap) out[len] = '\0';
  return len >= 0 && len < cap ? len : -1;
}

}  // namespace report

namespace webfmt {

namespace {

bool endsWith(const char* s, const char* suffix) {
  const size_t n = std::strlen(s);
  const size_t k = std::strlen(suffix);
  return n >= k && std::strcmp(s + n - k, suffix) == 0;
}

bool startsWith(const char* s, const char* prefix) {
  return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

// Days since 1970-01-01 to the civil date (H. Hinnant's algorithm)
void civil(int64_t days, int* y, int* m, int* d) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const int64_t doe = days - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  *d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  *m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  *y = static_cast<int>(yoe + era * 400 + (*m <= 2 ? 1 : 0));
}

void split(int64_t epochS, int64_t* days, int* hh, int* mm, int* ss) {
  int64_t d = epochS / 86400;
  int64_t r = epochS % 86400;
  if (r < 0) {
    r += 86400;
    --d;
  }
  *days = d;
  *hh = static_cast<int>(r / 3600);
  *mm = static_cast<int>(r / 60 % 60);
  *ss = static_cast<int>(r % 60);
}

void put2(char* out, int cap, int* len, int v) {
  const char b[3] = {static_cast<char>('0' + v / 10 % 10), static_cast<char>('0' + v % 10), 0};
  put(out, cap, len, b);
}

const char kHex[] = "0123456789ABCDEF";       // %XX in paths
const char kHexLower[] = "0123456789abcdef";  // \u00xx in JSON, as escapeForScript

// Path for <D:href>: unreserved characters and '/' as they are, others %XX
void putHref(char* out, int cap, int* len, const char* s) {
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
    const unsigned char c = *p;
    const bool plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       c == '/' || c == '.' || c == '_' || c == '-' || c == '~';
    if (plain) {
      const char b[2] = {static_cast<char>(c), 0};
      put(out, cap, len, b);
    } else {
      const char b[4] = {'%', kHex[c >> 4], kHex[c & 15], 0};
      put(out, cap, len, b);
    }
  }
}

}  // namespace

void putN(char* out, int cap, int* len, const char* s, int n) {
  if (*len < 0) return;
  if (n < 0 || *len + n >= cap) {  // keep room for a terminating NUL
    *len = -1;
    return;
  }
  std::memcpy(out + *len, s, static_cast<size_t>(n));
  *len += n;
  out[*len] = '\0';
}

void put(char* out, int cap, int* len, const char* s) { putN(out, cap, len, s, static_cast<int>(std::strlen(s))); }

void putInt(char* out, int cap, int* len, int64_t v) {
  char b[24];
  int i = 23;
  b[i] = '\0';
  const bool neg = v < 0;
  uint64_t u = neg ? static_cast<uint64_t>(-(v + 1)) + 1u : static_cast<uint64_t>(v);
  do {
    b[--i] = static_cast<char>('0' + u % 10u);
    u /= 10u;
  } while (u);
  if (neg) b[--i] = '-';
  put(out, cap, len, b + i);
}

void putJsonString(char* out, int cap, int* len, const char* s) {
  put(out, cap, len, "\"");
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
    const unsigned char c = *p;
    if (c == '"') put(out, cap, len, "\\\"");
    else if (c == '\\') put(out, cap, len, "\\\\");
    else if (c == '\n') put(out, cap, len, "\\n");
    else if (c == '\r') put(out, cap, len, "\\r");
    else if (c == '\t') put(out, cap, len, "\\t");
    else if (c < 0x20 || c == '<' || c == 0x7F) {
      const char b[7] = {'\\', 'u', '0', '0', kHexLower[c >> 4], kHexLower[c & 15], 0};
      put(out, cap, len, b);
    } else {
      const char b[2] = {static_cast<char>(c), 0};
      put(out, cap, len, b);
    }
  }
  put(out, cap, len, "\"");
}

void putXml(char* out, int cap, int* len, const char* s) {
  for (const char* p = s; *p; ++p) {
    switch (*p) {
      case '&': put(out, cap, len, "&amp;"); break;
      case '<': put(out, cap, len, "&lt;"); break;
      case '>': put(out, cap, len, "&gt;"); break;
      case '"': put(out, cap, len, "&quot;"); break;
      default: {
        const char b[2] = {*p, 0};
        put(out, cap, len, b);
      }
    }
  }
}

bool isSessionId(const char* s) {
  if (std::strlen(s) != 15) return false;
  for (int i = 0; i < 15; ++i) {
    if (i == 8) {
      if (s[i] != '_') return false;
    } else if (s[i] < '0' || s[i] > '9') {
      return false;
    }
  }
  return true;
}

bool isFileName(const char* s) {
  const size_t n = std::strlen(s);
  if (n == 0 || n > 63 || s[0] == '.') return false;
  for (size_t i = 0; i < n; ++i) {
    const char c = s[i];
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

bool isConfigName(const char* s) {
  return std::strcmp(s, "courses.json") == 0 || std::strcmp(s, "maint.json") == 0 ||
         std::strcmp(s, "checklist.json") == 0 || std::strcmp(s, "wifi.json") == 0;
}

FileKind kindOf(const char* name) {
  if (startsWith(name, kMarkerPrefix)) return FileKind::Marker;
  if (name[0] == '.' || std::strcmp(name, "System Volume Information") == 0) return FileKind::Hidden;
  if (std::strcmp(name, "meta.json") == 0) return FileKind::Meta;
  if (endsWith(name, "_laps.csv")) return FileKind::Laps;
  if (endsWith(name, "_imu.csv")) return FileKind::Imu;
  if (endsWith(name, "_can.csv")) return FileKind::Can;
  if (endsWith(name, ".csv")) return FileKind::MainCsv;
  if (endsWith(name, ".vbo")) return FileKind::Vbo;
  if (endsWith(name, ".ubx")) return FileKind::Ubx;
  return FileKind::Other;
}

bool countsAsDownload(FileKind k) { return k == FileKind::MainCsv || k == FileKind::Vbo; }

const char* contentType(const char* name) {
  if (endsWith(name, ".csv")) return "text/csv; charset=utf-8";
  if (endsWith(name, ".json")) return "application/json; charset=utf-8";
  if (endsWith(name, ".html")) return "text/html; charset=utf-8";
  return "application/octet-stream";
}

int httpDate(char* out, int cap, int64_t epochS) {
  static const char* const kDay[7] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
  static const char* const kMon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int64_t days = 0;
  int hh = 0, mm = 0, ss = 0, y = 0, m = 0, d = 0;
  split(epochS, &days, &hh, &mm, &ss);
  civil(days, &y, &m, &d);
  int wd = static_cast<int>(days % 7);
  if (wd < 0) wd += 7;
  int len = 0;
  if (cap <= 0) return -1;
  out[0] = '\0';
  put(out, cap, &len, kDay[wd]);
  put(out, cap, &len, ", ");
  put2(out, cap, &len, d);
  put(out, cap, &len, " ");
  put(out, cap, &len, kMon[m - 1]);
  put(out, cap, &len, " ");
  putInt(out, cap, &len, y);
  put(out, cap, &len, " ");
  put2(out, cap, &len, hh);
  put(out, cap, &len, ":");
  put2(out, cap, &len, mm);
  put(out, cap, &len, ":");
  put2(out, cap, &len, ss);
  put(out, cap, &len, " GMT");
  return len;
}

int isoDate(char* out, int cap, int64_t epochS) {
  int64_t days = 0;
  int hh = 0, mm = 0, ss = 0, y = 0, m = 0, d = 0;
  split(epochS, &days, &hh, &mm, &ss);
  civil(days, &y, &m, &d);
  int len = 0;
  if (cap <= 0) return -1;
  out[0] = '\0';
  putInt(out, cap, &len, y);
  put(out, cap, &len, "-");
  put2(out, cap, &len, m);
  put(out, cap, &len, "-");
  put2(out, cap, &len, d);
  put(out, cap, &len, "T");
  put2(out, cap, &len, hh);
  put(out, cap, &len, ":");
  put2(out, cap, &len, mm);
  put(out, cap, &len, ":");
  put2(out, cap, &len, ss);
  put(out, cap, &len, "Z");
  return len;
}

int davResponse(char* out, int cap, const DavEntry& e) {
  int len = 0;
  if (cap <= 0) return -1;
  out[0] = '\0';
  put(out, cap, &len, "<D:response><D:href>");
  putHref(out, cap, &len, e.href);
  put(out, cap, &len, "</D:href><D:propstat><D:prop><D:displayname>");
  putXml(out, cap, &len, e.name);
  put(out, cap, &len, "</D:displayname>");
  if (e.collection) {
    put(out, cap, &len, "<D:resourcetype><D:collection/></D:resourcetype>");
    if (e.quotaFree >= 0) {
      put(out, cap, &len, "<D:quota-available-bytes>");
      putInt(out, cap, &len, e.quotaFree);
      put(out, cap, &len, "</D:quota-available-bytes>");
    }
    if (e.quotaUsed >= 0) {
      put(out, cap, &len, "<D:quota-used-bytes>");
      putInt(out, cap, &len, e.quotaUsed);
      put(out, cap, &len, "</D:quota-used-bytes>");
    }
  } else {
    put(out, cap, &len, "<D:resourcetype/><D:getcontentlength>");
    putInt(out, cap, &len, e.size);
    put(out, cap, &len, "</D:getcontentlength><D:getcontenttype>");
    putXml(out, cap, &len, contentType(e.name));
    put(out, cap, &len, "</D:getcontenttype>");
  }
  if (e.mtimeS > 0) {
    char b[40];
    if (httpDate(b, sizeof b, e.mtimeS) > 0) {
      put(out, cap, &len, "<D:getlastmodified>");
      put(out, cap, &len, b);
      put(out, cap, &len, "</D:getlastmodified>");
    }
    if (isoDate(b, sizeof b, e.mtimeS) > 0) {
      put(out, cap, &len, "<D:creationdate>");
      put(out, cap, &len, b);
      put(out, cap, &len, "</D:creationdate>");
    }
  }
  put(out, cap, &len, "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>\n");
  return len;
}

}  // namespace webfmt
}  // namespace lt
