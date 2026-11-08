#include "maint.h"

#include <cstring>

namespace lt {

// ---- dates (H. Hinnant's civil-from-days algorithms) --------------------------------------------

int32_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2 ? 1 : 0;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;                                   // [0, 399]
  const int doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;  // [0, 365]
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
  return era * 146097 + doe - 719468;
}

void civilFromDays(int32_t day, int* y, int* m, int* d) {
  const int z = day + 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const int doe = z - era * 146097;                                     // [0, 146096]
  const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
  const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);              // [0, 365]
  const int mp = (5 * doy + 2) / 153;                                   // [0, 11]
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = yoe + era * 400 + (*m <= 2 ? 1 : 0);
}

namespace {

int daysInMonth(int y, int m) {
  static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  return m == 2 && leap ? 29 : kDays[m - 1];
}

bool digitsAt(const char* s, int from, int count, int* v) {
  int x = 0;
  for (int i = from; i < from + count; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    x = x * 10 + (s[i] - '0');
  }
  *v = x;
  return true;
}

}  // namespace

bool parseDate(const char* s, int n, int32_t* day) {
  if (!s || n != 10 || s[4] != '-' || s[7] != '-') return false;
  int y = 0, m = 0, d = 0;
  if (!digitsAt(s, 0, 4, &y) || !digitsAt(s, 5, 2, &m) || !digitsAt(s, 8, 2, &d)) return false;
  if (y < 1900 || m < 1 || m > 12 || d < 1 || d > daysInMonth(y, m)) return false;
  *day = daysFromCivil(y, m, d);
  return true;
}

int formatDate(char* out, int cap, int32_t day) {
  if (cap < 11 || day == kNoDate) return 0;
  int y = 0, m = 0, d = 0;
  civilFromDays(day, &y, &m, &d);
  if (y < 0 || y > 9999) return 0;
  const int v[3] = {y, m, d};
  char* p = out;
  for (int k = 0; k < 3; ++k) {
    const int width = k == 0 ? 4 : 2;
    int x = v[k];
    for (int i = width - 1; i >= 0; --i) {
      p[i] = static_cast<char>('0' + x % 10);
      x /= 10;
    }
    p += width;
    if (k < 2) *p++ = '-';
  }
  *p = '\0';
  return 10;
}

int32_t dayOfUtcMs(int64_t utcMs, int tzMin) {
  const int64_t local = utcMs + static_cast<int64_t>(tzMin) * 60000;
  const int64_t msPerDay = 86400000;
  int64_t q = local / msPerDay;
  if (local % msPerDay < 0) --q;  // floor for times before 1970
  return static_cast<int32_t>(q);
}

// ---- maint.json ------------------------------------------------------------------------------------

namespace {

bool validId(const char* id) {
  int n = 0;
  for (; id[n]; ++n) {
    const char c = id[n];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return n > 0 && n < 24;
}

// A number >= 0, or null (= not recorded)
bool readOptional(json::Reader& r, bool* has, float* v, const char* why) {
  if (r.peek() == json::Type::Null) {
    *has = false;
    return r.skip();
  }
  float x = 0.0f;
  if (!r.readFloat(&x)) return false;
  if (!(x >= 0.0f)) return r.fail(why);
  *has = true;
  *v = x;
  return true;
}

bool readItem(json::Reader& r, MaintItem* it) {
  *it = MaintItem{};
  it->lastDay = kNoDate;
  bool haveId = false;
  bool haveName = false;
  if (!r.beginObject()) return false;
  while (r.nextKey()) {
    if (r.keyIs("id")) {
      const int n = r.readString(it->id, sizeof it->id);
      if (n < 0) return false;
      if (n >= static_cast<int>(sizeof it->id) || !validId(it->id)) return r.fail("bad-id");
      haveId = true;
    } else if (r.keyIs("name")) {
      const int n = r.readString(it->name, sizeof it->name);
      if (n < 0) return false;
      if (n >= static_cast<int>(sizeof it->name)) return r.fail("name-too-long");
      haveName = true;
    } else if (r.keyIs("note")) {
      const int n = r.readString(it->note, sizeof it->note);
      if (n < 0) return false;
      if (n >= static_cast<int>(sizeof it->note)) return r.fail("note-too-long");
    } else if (r.keyIs("interval_km") || r.keyIs("interval_days")) {
      int32_t v = 0;
      if (!r.readInt(&v)) return false;
      if (v < 0) return r.fail("bad-interval");
      if (r.keyIs("interval_km")) {
        it->intervalKm = v;
      } else {
        it->intervalDays = v;
      }
    } else if (r.keyIs("interval_track_h")) {
      float v = 0.0f;
      if (!r.readFloat(&v)) return false;
      if (!(v >= 0.0f)) return r.fail("bad-interval");
      it->intervalTrackH = v;
    } else if (r.keyIs("last_date")) {
      if (r.peek() == json::Type::Null) {
        if (!r.skip()) return false;
        continue;
      }
      char t[16];
      const int n = r.readString(t, sizeof t);
      if (n < 0) return false;
      if (n == 0) continue;  // "" = not recorded
      if (n >= static_cast<int>(sizeof t) || !parseDate(t, n, &it->lastDay)) return r.fail("bad-date");
    } else if (r.keyIs("last_odo")) {
      if (!readOptional(r, &it->hasOdo, &it->lastOdoKm, "bad-odo")) return false;
    } else if (r.keyIs("last_track_h")) {
      if (!readOptional(r, &it->hasTrackH, &it->lastTrackH, "bad-track-h")) return false;
    } else if (!r.skip()) {
      return false;
    }
  }
  if (!r.ok()) return false;
  if (!haveId) return r.fail("missing-id");
  if (!haveName) std::memcpy(it->name, it->id, sizeof it->id);
  return true;
}

bool readDoc(json::Reader& r, MaintItem* store, int* count) {
  char ids[kMaintMaxItems][24];
  int n = 0;
  bool haveVersion = false;
  bool haveItems = false;
  MaintItem tmp;
  if (!r.beginObject()) return false;
  while (r.nextKey()) {
    if (r.keyIs("version")) {
      int32_t v = 0;
      if (!r.readInt(&v)) return false;
      if (v != 1) return r.fail("bad-version");
      haveVersion = true;
    } else if (r.keyIs("items")) {
      if (!r.beginArray()) return false;
      while (r.nextItem()) {
        if (n >= kMaintMaxItems) return r.fail("too-many-items");
        MaintItem* it = store ? &store[n] : &tmp;
        if (!readItem(r, it)) return false;
        for (int i = 0; i < n; ++i) {
          if (std::strcmp(ids[i], it->id) == 0) return r.fail("duplicate-id");
        }
        std::memcpy(ids[n], it->id, sizeof it->id);
        ++n;
      }
      if (!r.ok()) return false;
      haveItems = true;
    } else if (!r.skip()) {
      return false;
    }
  }
  if (!r.finish()) return false;
  if (!haveVersion) return r.fail("missing-version");
  if (!haveItems) return r.fail("missing-items");
  *count = n;
  return true;
}

int rank(const MaintStatus& s) { return static_cast<int>(s.state); }

// a before b in the order of urgency
bool before(const MaintStatus& a, const MaintStatus& b) {
  if (rank(a) != rank(b)) return rank(a) < rank(b);
  if (a.state != MaintState::Unknown && a.fraction != b.fraction) return a.fraction < b.fraction;
  return a.index < b.index;
}

}  // namespace

bool Maint::load(json::Source& src, json::Error* err) {
  n_ = 0;
  json::Reader r(src);
  int n = 0;
  const bool ok = readDoc(r, items_, &n);
  n_ = ok ? n : 0;
  if (err) *err = ok ? json::Error{0, 0, ""} : r.error();
  return ok;
}

bool Maint::load(const char* text, int len, json::Error* err) {
  json::MemSource src(text, len);
  return load(src, err);
}

bool Maint::check(json::Source& src, json::Error* err) {
  json::Reader r(src);
  int n = 0;
  const bool ok = readDoc(r, nullptr, &n);
  if (err) *err = ok ? json::Error{0, 0, ""} : r.error();
  return ok;
}

int Maint::find(const char* id) const {
  for (int i = 0; i < n_; ++i) {
    if (std::strcmp(items_[i].id, id) == 0) return i;
  }
  return -1;
}

MaintStatus Maint::status(int i, const MaintNow& now) const {
  const MaintItem& it = items_[i];
  MaintStatus s{};
  s.index = i;
  s.state = MaintState::Unknown;
  s.unit = MaintUnit::None;
  if (it.intervalKm > 0 && it.hasOdo && now.odoOk) {
    s.hasKm = true;
    s.remKm = it.lastOdoKm + static_cast<float>(it.intervalKm) - now.odoKm;
    s.fracKm = s.remKm / static_cast<float>(it.intervalKm);
  }
  if (it.intervalDays > 0 && it.lastDay != kNoDate && now.today != kNoDate) {
    s.hasDays = true;
    s.remDays = static_cast<float>(static_cast<int64_t>(it.lastDay) + it.intervalDays - now.today);
    s.fracDays = s.remDays / static_cast<float>(it.intervalDays);
  }
  if (it.intervalTrackH > 0.0f && it.hasTrackH && now.trackOk) {
    s.hasTrackH = true;
    s.remTrackH = it.lastTrackH + it.intervalTrackH - now.trackH;
    s.fracTrackH = s.remTrackH / it.intervalTrackH;
  }
  // the interval with the smallest fraction left (ties: km, days, hours)
  bool any = false;
  auto take = [&](bool has, MaintUnit u, float rem, float frac) {
    if (!has || (any && frac >= s.fraction)) return;
    any = true;
    s.unit = u;
    s.remaining = rem;
    s.fraction = frac;
  };
  take(s.hasKm, MaintUnit::Km, s.remKm, s.fracKm);
  take(s.hasDays, MaintUnit::Days, s.remDays, s.fracDays);
  take(s.hasTrackH, MaintUnit::TrackH, s.remTrackH, s.fracTrackH);
  if (any) {
    s.state = s.fraction <= 0.0f ? MaintState::Overdue
              : s.fraction <= kMaintWarnFraction ? MaintState::Warn
                                                 : MaintState::Ok;
  }
  return s;
}

int Maint::evaluate(const MaintNow& now, MaintStatus* out, int cap) const {
  MaintStatus all[kMaintMaxItems];
  for (int i = 0; i < n_; ++i) {
    const MaintStatus s = status(i, now);
    int k = i;
    while (k > 0 && before(s, all[k - 1])) {  // insertion sort (stable)
      all[k] = all[k - 1];
      --k;
    }
    all[k] = s;
  }
  const int n = n_ < cap ? n_ : cap;
  for (int i = 0; i < n; ++i) out[i] = all[i];
  return n;
}

bool Maint::markDone(const char* id, const MaintNow& now) {
  const int i = find(id);
  if (i < 0 || now.today == kNoDate) return false;
  MaintItem& it = items_[i];
  it.lastDay = now.today;
  it.hasOdo = now.odoOk;
  it.lastOdoKm = now.odoOk ? now.odoKm : 0.0f;
  it.hasTrackH = now.trackOk;
  it.lastTrackH = now.trackOk ? now.trackH : 0.0f;
  return true;
}

int Maint::toJson(char* out, int cap) const {
  json::Writer w(out, cap);
  w.beginObject().key("version").i64(1).key("items").beginArray();
  for (int i = 0; i < n_; ++i) {
    const MaintItem& it = items_[i];
    char date[12] = "";
    formatDate(date, sizeof date, it.lastDay);
    w.beginObject(true).key("id").str(it.id).key("name").str(it.name);
    w.key("interval_km").i64(it.intervalKm).key("interval_days").i64(it.intervalDays);
    w.key("interval_track_h").num(it.intervalTrackH, 2);
    w.key("last_date").str(date);
    w.key("last_odo");
    if (it.hasOdo) {
      w.num(it.lastOdoKm, 1);
    } else {
      w.null();
    }
    w.key("last_track_h");
    if (it.hasTrackH) {
      w.num(it.lastTrackH, 2);
    } else {
      w.null();
    }
    w.key("note").str(it.note).endObject();
  }
  w.endArray().endObject();
  return w.finish();
}

}  // namespace lt
