#include "course.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace lt {

namespace {

constexpr float kMPerE7 = 0.011119493f;  // metres per 1e-7 degree (6371 km sphere); 3 km rule only
constexpr float kDegToRadF = 0.017453292519943295f;
constexpr int32_t kLatMaxE7 = 900000000;
constexpr int32_t kLonMaxE7 = 1800000000;

bool latOk(int32_t v) { return v >= -kLatMaxE7 && v <= kLatMaxE7; }
bool lonOk(int32_t v) { return v >= -kLonMaxE7 && v <= kLonMaxE7; }

// Copies at most cap - 1 bytes of s[0..n), cut at a UTF-8 character boundary, NUL-terminated
void copyUtf8(char* dst, int cap, const char* s, int n) {
  if (cap <= 0) return;
  int k = n < cap - 1 ? n : cap - 1;
  if (k < n) {
    while (k > 0 && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80) --k;
  }
  std::memcpy(dst, s, static_cast<size_t>(k));
  dst[k] = '\0';
}

void computeCenter(Course* c) {
  if (c->centerGiven || c->numGates == 0) return;
  int64_t lat = 0;
  int64_t lon = 0;
  for (int i = 0; i < c->numGates; ++i) {
    lat += c->gates[i].latE7;
    lon += c->gates[i].lonE7;
  }
  c->centerLatE7 = static_cast<int32_t>(lat / c->numGates);
  c->centerLonE7 = static_cast<int32_t>(lon / c->numGates);
}

bool readGate(json::Reader& r, CourseGate* g) {
  *g = CourseGate{};
  bool haveType = false, haveLat = false, haveLon = false, haveHeading = false, haveWidth = false;
  if (!r.beginObject()) return false;
  while (r.nextKey()) {
    if (r.keyIs("type")) {
      char t[8];
      const int n = r.readString(t, sizeof t);
      if (n < 0) return false;
      if (n < static_cast<int>(sizeof t) && std::strcmp(t, "sf") == 0) {
        g->type = GateType::Sf;
      } else if (n < static_cast<int>(sizeof t) && std::strcmp(t, "split") == 0) {
        g->type = GateType::Split;
      } else {
        return r.fail("bad-gate-type");
      }
      haveType = true;
    } else if (r.keyIs("name")) {
      const int n = r.readString(g->name, sizeof g->name);
      if (n < 0) return false;
      if (n >= static_cast<int>(sizeof g->name)) return r.fail("gate-name-too-long");
    } else if (r.keyIs("lat")) {
      if (!r.readE7(&g->latE7)) return false;
      if (!latOk(g->latE7)) return r.fail("bad-lat");
      haveLat = true;
    } else if (r.keyIs("lon")) {
      if (!r.readE7(&g->lonE7)) return false;
      if (!lonOk(g->lonE7)) return r.fail("bad-lon");
      haveLon = true;
    } else if (r.keyIs("heading")) {
      float h = 0.0f;
      if (!r.readFloat(&h)) return false;
      if (!(h >= 0.0f && h <= 360.0f)) return r.fail("bad-heading");
      g->headingDeg = h >= 360.0f ? 0.0f : h;
      haveHeading = true;
    } else if (r.keyIs("width")) {
      float w = 0.0f;
      if (!r.readFloat(&w)) return false;
      if (!(w > 0.0f && w <= 200.0f)) return r.fail("bad-width");
      g->widthM = w;
      haveWidth = true;
    } else if (!r.skip()) {
      return false;
    }
  }
  if (!r.ok()) return false;
  if (!haveType) return r.fail("missing-type");
  if (!haveLat) return r.fail("missing-lat");
  if (!haveLon) return r.fail("missing-lon");
  if (!haveHeading) return r.fail("missing-heading");
  if (!haveWidth) g->widthM = g->type == GateType::Sf ? kSfWidthM : kSplitWidthM;
  return true;
}

bool readCourse(json::Reader& r, Course* c) {
  *c = Course{};
  c->minLapS = kDefaultMinLapS;
  bool haveId = false;
  bool haveName = false;
  int numSf = 0;
  if (!r.beginObject()) return false;
  while (r.nextKey()) {
    if (r.keyIs("id")) {
      const int n = r.readString(c->id, sizeof c->id);
      if (n < 0) return false;
      if (n >= static_cast<int>(sizeof c->id) || !CourseDb::validId(c->id)) return r.fail("bad-id");
      haveId = true;
    } else if (r.keyIs("name")) {
      const int n = r.readString(c->name, sizeof c->name);
      if (n < 0) return false;
      if (n >= static_cast<int>(sizeof c->name)) return r.fail("name-too-long");
      haveName = true;
    } else if (r.keyIs("min_lap_s")) {
      float v = 0.0f;
      if (!r.readFloat(&v)) return false;
      if (!(v >= 5.0f && v <= 3600.0f)) return r.fail("bad-min-lap");
      c->minLapS = v;
    } else if (r.keyIs("center")) {
      int32_t lat = 0;
      int32_t lon = 0;
      if (!r.beginArray()) return false;
      if (!r.nextItem()) return r.ok() ? r.fail("bad-center") : false;
      if (!r.readE7(&lat)) return false;
      if (!r.nextItem()) return r.ok() ? r.fail("bad-center") : false;
      if (!r.readE7(&lon)) return false;
      if (r.nextItem()) return r.fail("bad-center");
      if (!r.ok()) return false;
      if (!latOk(lat) || !lonOk(lon)) return r.fail("bad-center");
      c->centerLatE7 = lat;
      c->centerLonE7 = lon;
      c->centerGiven = true;
    } else if (r.keyIs("gates")) {
      if (!r.beginArray()) return false;
      while (r.nextItem()) {
        if (c->numGates >= kCourseMaxGates) return r.fail("too-many-gates");
        CourseGate& g = c->gates[c->numGates];
        if (!readGate(r, &g)) return false;
        if (g.type == GateType::Sf && ++numSf > 1) return r.fail("two-sf");
        ++c->numGates;
      }
      if (!r.ok()) return false;
    } else if (!r.skip()) {
      return false;
    }
  }
  if (!r.ok()) return false;
  if (!haveId) return r.fail("missing-id");
  if (numSf == 0) return r.fail("no-sf");
  if (!haveName) std::memcpy(c->name, c->id, sizeof c->id);
  computeCenter(c);
  return true;
}

// store == null: checks only (one course at a time in a local)
bool readDoc(json::Reader& r, Course* store, int* count) {
  char ids[kMaxCourses][32];
  int n = 0;
  bool haveVersion = false;
  bool haveCourses = false;
  Course tmp;
  if (!r.beginObject()) return false;
  while (r.nextKey()) {
    if (r.keyIs("version")) {
      int32_t v = 0;
      if (!r.readInt(&v)) return false;
      if (v != 1) return r.fail("bad-version");
      haveVersion = true;
    } else if (r.keyIs("courses")) {
      if (!r.beginArray()) return false;
      while (r.nextItem()) {
        if (n >= kMaxCourses) return r.fail("too-many-courses");
        Course* c = store ? &store[n] : &tmp;
        if (!readCourse(r, c)) return false;
        for (int i = 0; i < n; ++i) {
          if (std::strcmp(ids[i], c->id) == 0) return r.fail("duplicate-id");
        }
        std::memcpy(ids[n], c->id, sizeof c->id);
        ++n;
      }
      if (!r.ok()) return false;
      haveCourses = true;
    } else if (!r.skip()) {
      return false;
    }
  }
  if (!r.finish()) return false;
  if (!haveVersion) return r.fail("missing-version");
  if (!haveCourses) return r.fail("missing-courses");
  *count = n;
  return true;
}

bool validCourse(const Course& c) {
  if (!CourseDb::validId(c.id) || c.numGates < 1 || c.numGates > kCourseMaxGates) return false;
  if (!(c.minLapS > 0.0f)) return false;
  int sf = 0;
  for (int i = 0; i < c.numGates; ++i) {
    const CourseGate& g = c.gates[i];
    if (g.type == GateType::Sf) ++sf;
    if (!latOk(g.latE7) || !lonOk(g.lonE7) || !(g.widthM > 0.0f)) return false;
    if (!(g.headingDeg >= 0.0f && g.headingDeg < 360.0f)) return false;
  }
  return sf == 1;
}

// ---- RaceChrono [laptiming] ---------------------------------------------------------------------

bool lineIs(const char* s, const char* e, const char* word) {
  const size_t n = std::strlen(word);
  return static_cast<size_t>(e - s) == n && std::memcmp(s, word, n) == 0;
}

void skipBlank(const char** p, const char* e) {
  while (*p < e && (**p == ' ' || **p == '\t')) ++*p;
}

// Next token up to a blank
bool token(const char** p, const char* e, const char** s, int* n) {
  skipBlank(p, e);
  *s = *p;
  while (*p < e && **p != ' ' && **p != '\t') ++*p;
  *n = static_cast<int>(*p - *s);
  return *n > 0;
}

// Minutes as text -> 1e-7 degrees, rounded (integer arithmetic: 1e-8 minutes / 600)
bool minutesToE7(const char* s, int n, int64_t* e7) {
  int64_t v = 0;
  if (!json::parseScaled(s, n, 8, &v)) return false;
  const int64_t a = v < 0 ? -v : v;
  const int64_t q = (a + 300) / 600;
  *e7 = v < 0 ? -q : q;
  return true;
}

// "Start   -8405.283480 +2123.694540 -8405.257911 +2123.659851 ¬ Start/Finish"
bool laptimingGate(const char* s, const char* e, GateType type, CourseGate* g) {
  const char* t = nullptr;
  int n = 0;
  token(&s, e, &t, &n);  // the kind
  int64_t v[4];
  for (int k = 0; k < 4; ++k) {
    if (!token(&s, e, &t, &n) || !minutesToE7(t, n, &v[k])) return false;
  }
  const int64_t lonA = -v[0], latA = v[1], lonB = -v[2], latB = v[3];  // longitude: west positive
  if (latA < -kLatMaxE7 || latA > kLatMaxE7 || latB < -kLatMaxE7 || latB > kLatMaxE7) return false;
  if (lonA < -kLonMaxE7 || lonA > kLonMaxE7 || lonB < -kLonMaxE7 || lonB > kLonMaxE7) return false;
  *g = CourseGate{};
  g->type = type;
  g->latE7 = static_cast<int32_t>(latA);
  g->lonE7 = static_cast<int32_t>(lonA);
  Plane pl;
  pl.init(g->latE7, g->lonE7);
  const Vec2 b = pl.toLocal(static_cast<int32_t>(latB), static_cast<int32_t>(lonB));
  if (length(b) < 0.5f) return false;  // no direction
  g->headingDeg = headingOf({-b.x, -b.y});  // B -> A
  g->widthM = type == GateType::Sf ? kSfWidthM : kSplitWidthM;
  // the name: after the separator (U+00AC "¬" in RaceChrono's export)
  skipBlank(&s, e);
  if (e - s >= 2 && static_cast<unsigned char>(s[0]) == 0xC2 && static_cast<unsigned char>(s[1]) == 0xAC) s += 2;
  skipBlank(&s, e);
  copyUtf8(g->name, sizeof g->name, s, static_cast<int>(e - s));
  return true;
}

void makeId(const Course& c, char* id, int cap) {
  int k = 0;
  bool gap = false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(c.name); *p && k < cap - 1; ++p) {
    const unsigned char ch = *p;
    if (ch >= 0x80) continue;  // not ASCII
    const bool alnum = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
    if (!alnum) {
      gap = true;
      continue;
    }
    if (gap && k > 0 && k < cap - 2) id[k++] = '_';
    gap = false;
    id[k++] = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
  }
  id[k] = '\0';
  if (k > 0) return;
  const CourseGate* sf = nullptr;
  for (int i = 0; i < c.numGates; ++i) {
    if (c.gates[i].type == GateType::Sf) sf = &c.gates[i];
  }
  const int32_t lat = sf ? sf->latE7 : 0;
  const int32_t lon = sf ? sf->lonE7 : 0;
  std::snprintf(id, static_cast<size_t>(cap), "course_%c%ld_%c%ld", lat < 0 ? 's' : 'n',
                static_cast<long>((lat < 0 ? -static_cast<int64_t>(lat) : lat) / 10000), lon < 0 ? 'w' : 'e',
                static_cast<long>((lon < 0 ? -static_cast<int64_t>(lon) : lon) / 10000));
}

}  // namespace

// ---- CourseDb ------------------------------------------------------------------------------------

bool CourseDb::validId(const char* id) {
  if (!id) return false;
  int n = 0;
  for (; id[n]; ++n) {
    const char c = id[n];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    if (n >= 31) return false;
  }
  return n > 0;
}

bool CourseDb::load(json::Source& src, json::Error* err) {
  n_ = 0;
  json::Reader r(src);
  int n = 0;
  const bool ok = readDoc(r, c_, &n);
  n_ = ok ? n : 0;
  if (err) *err = ok ? json::Error{0, 0, ""} : r.error();
  return ok;
}

bool CourseDb::load(const char* text, int len, json::Error* err) {
  json::MemSource src(text, len);
  return load(src, err);
}

bool CourseDb::check(json::Source& src, json::Error* err) {
  json::Reader r(src);
  int n = 0;
  const bool ok = readDoc(r, nullptr, &n);
  if (err) *err = ok ? json::Error{0, 0, ""} : r.error();
  return ok;
}

int CourseDb::find(const char* id) const {
  for (int i = 0; i < n_; ++i) {
    if (std::strcmp(c_[i].id, id) == 0) return i;
  }
  return -1;
}

bool CourseDb::put(const Course& c) {
  if (!validCourse(c)) return false;
  int i = find(c.id);
  if (i < 0) {
    if (n_ >= kMaxCourses) return false;
    i = n_++;
  }
  c_[i] = c;
  computeCenter(&c_[i]);
  return true;
}

bool CourseDb::remove(const char* id) {
  const int i = find(id);
  if (i < 0) return false;
  for (int k = i; k + 1 < n_; ++k) c_[k] = c_[k + 1];
  --n_;
  return true;
}

float CourseDb::distanceM(int i, int32_t latE7, int32_t lonE7) const {
  const Course& c = c_[i];
  int64_t dLon = static_cast<int64_t>(lonE7) - c.centerLonE7;
  if (dLon > kLonMaxE7) dLon -= 2 * static_cast<int64_t>(kLonMaxE7);
  if (dLon < -kLonMaxE7) dLon += 2 * static_cast<int64_t>(kLonMaxE7);
  const int64_t dLat = static_cast<int64_t>(latE7) - c.centerLatE7;
  const float cosLat = std::cos(static_cast<float>(c.centerLatE7) * 1e-7f * kDegToRadF);
  const float dx = static_cast<float>(dLon) * kMPerE7 * cosLat;
  const float dy = static_cast<float>(dLat) * kMPerE7;
  return std::sqrt(dx * dx + dy * dy);
}

int CourseDb::nearest(int32_t latE7, int32_t lonE7, float maxM, float* distM) const {
  int best = -1;
  float bestD = 0.0f;
  for (int i = 0; i < n_; ++i) {
    const float d = distanceM(i, latE7, lonE7);
    if (d <= maxM && (best < 0 || d < bestD)) {
      best = i;
      bestD = d;
    }
  }
  if (distM) *distM = best >= 0 ? bestD : -1.0f;
  return best;
}

bool CourseDb::buildCourseDef(int i, CourseDef* out) const {
  if (i < 0 || i >= n_) return false;
  const Course& c = c_[i];
  CourseDef d{};
  bool haveSf = false;
  for (int k = 0; k < c.numGates; ++k) {
    const CourseGate& g = c.gates[k];
    const GateDef gd = {g.latE7, g.lonE7, g.headingDeg, g.widthM};
    if (g.type == GateType::Sf) {
      d.startFinish = gd;
      haveSf = true;
    } else if (d.numSectors < kMaxSectors) {
      d.sectors[d.numSectors++] = gd;
    }
  }
  d.minLapS = c.minLapS;
  if (!haveSf) return false;
  *out = d;
  return true;
}

void CourseDb::initPlane(int i, Plane* pl) const { pl->init(c_[i].centerLatE7, c_[i].centerLonE7); }

int CourseDb::writeJson(const Course* courses, int n, char* out, int cap) {
  json::Writer w(out, cap);
  w.beginObject().key("version").i64(1).key("courses").beginArray();
  for (int i = 0; i < n; ++i) {
    const Course& c = courses[i];
    w.beginObject().key("id").str(c.id).key("name").str(c.name).key("min_lap_s").num(c.minLapS, 2);
    if (c.centerGiven) {
      w.key("center").beginArray(true).scaled(c.centerLatE7, 7).scaled(c.centerLonE7, 7).endArray();
    }
    w.key("gates").beginArray();
    for (int k = 0; k < c.numGates; ++k) {
      const CourseGate& g = c.gates[k];
      w.beginObject(true).key("type").str(g.type == GateType::Sf ? "sf" : "split");
      if (g.name[0]) w.key("name").str(g.name);
      w.key("lat").scaled(g.latE7, 7).key("lon").scaled(g.lonE7, 7);
      w.key("heading").num(g.headingDeg, 2).key("width").num(g.widthM, 1).endObject();
    }
    w.endArray().endObject();
  }
  w.endArray().endObject();
  return w.finish();
}

int CourseDb::toJson(char* out, int cap) const { return writeJson(c_, n_, out, cap); }

bool CourseDb::parseRaceChronoVbo(const char* text, int len, const char* id, Course* out, const char** why) {
  const char* unused = nullptr;
  if (!why) why = &unused;
  *why = "";
  if (!text) len = 0;
  if (len < 0) len = static_cast<int>(std::strlen(text));
  Course c{};
  c.minLapS = kDefaultMinLapS;
  bool inLaptiming = false;
  bool inSession = false;
  bool sawLaptiming = false;
  int numSf = 0;
  const char* p = text;
  const char* end = text + len;
  while (p < end) {
    const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
    const char* s = p;
    const char* e = nl ? nl : end;
    p = nl ? nl + 1 : end;
    while (e > s && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) --e;
    while (s < e && (*s == ' ' || *s == '\t')) ++s;
    if (s == e) continue;
    if (*s == '[') {
      if (lineIs(s, e, "[data]")) break;
      inLaptiming = lineIs(s, e, "[laptiming]");
      inSession = lineIs(s, e, "[session data]");
      sawLaptiming = sawLaptiming || inLaptiming;
      continue;
    }
    if (inSession && e - s > 5 && std::memcmp(s, "name ", 5) == 0) {
      const char* nm = s + 5;
      skipBlank(&nm, e);
      copyUtf8(c.name, sizeof c.name, nm, static_cast<int>(e - nm));
      continue;
    }
    if (!inLaptiming) continue;
    const bool start = e - s > 5 && std::memcmp(s, "Start", 5) == 0 && (s[5] == ' ' || s[5] == '\t');
    const bool split = e - s > 5 && std::memcmp(s, "Split", 5) == 0 && (s[5] == ' ' || s[5] == '\t');
    if (!start && !split) continue;  // other kinds are not used
    if (c.numGates >= kCourseMaxGates) {
      *why = "too-many-gates";
      return false;
    }
    if (start && ++numSf > 1) {
      *why = "two-start-lines";
      return false;
    }
    if (!laptimingGate(s, e, start ? GateType::Sf : GateType::Split, &c.gates[c.numGates])) {
      *why = "bad-laptiming-line";
      return false;
    }
    ++c.numGates;
  }
  if (!sawLaptiming) {
    *why = "no-laptiming";
    return false;
  }
  if (numSf == 0) {
    *why = "no-start-line";
    return false;
  }
  if (id && id[0]) {
    if (!validId(id)) {
      *why = "bad-id";
      return false;
    }
    std::memcpy(c.id, id, std::strlen(id) + 1);
  } else {
    makeId(c, c.id, sizeof c.id);
  }
  if (!c.name[0]) std::memcpy(c.name, c.id, sizeof c.id);
  computeCenter(&c);
  *out = c;
  return true;
}

int CourseDb::fromRaceChronoVbo(const char* text, int len, const char* id, char* out, int cap, const char** why) {
  Course c;
  if (!parseRaceChronoVbo(text, len, id, &c, why)) return 0;
  return writeJson(&c, 1, out, cap);
}

}  // namespace lt
