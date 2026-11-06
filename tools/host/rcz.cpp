#include "rcz.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rcz {

namespace {

bool readFile(const std::string& path, std::string* out) {
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) return false;
  out->clear();
  char buf[65536];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) out->append(buf, n);
  std::fclose(fp);
  return true;
}

// --- just enough JSON for session.json / sessionfragment.json ---------------------------

// Position after `"key":` inside [from, to), or npos
size_t valueAt(const std::string& s, size_t from, size_t to, const char* key) {
  const std::string k = std::string("\"") + key + "\":";
  const size_t p = s.find(k, from);
  if (p == std::string::npos || p >= to) return std::string::npos;
  return p + k.size();
}

bool intAt(const std::string& s, size_t from, size_t to, const char* key, int64_t* v) {
  const size_t p = valueAt(s, from, to, key);
  if (p == std::string::npos) return false;
  *v = std::strtoll(s.c_str() + p, nullptr, 10);
  return true;
}

bool boolAt(const std::string& s, size_t from, size_t to, const char* key, bool* v) {
  const size_t p = valueAt(s, from, to, key);
  if (p == std::string::npos) return false;
  *v = s.compare(p, 4, "true") == 0;
  return true;
}

bool stringAt(const std::string& s, size_t from, size_t to, const char* key, std::string* v) {
  const size_t p = valueAt(s, from, to, key);
  if (p == std::string::npos || s[p] != '"') return false;
  const size_t e = s.find('"', p + 1);
  if (e == std::string::npos) return false;
  *v = s.substr(p + 1, e - p - 1);
  return true;
}

// End (one past) of the {...} or [...] that starts at `open`, skipping strings
size_t matchClose(const std::string& s, size_t open) {
  int depth = 0;
  bool inStr = false;
  for (size_t i = open; i < s.size(); ++i) {
    const char c = s[i];
    if (inStr) {
      if (c == '\\') ++i;
      else if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') inStr = true;
    else if (c == '{' || c == '[') ++depth;
    else if (c == '}' || c == ']') {
      if (--depth == 0) return i + 1;
    }
  }
  return std::string::npos;
}

// Calls fn(begin, end) for every object of the array value of `key`
template <typename Fn>
bool eachObject(const std::string& s, const char* key, Fn fn) {
  const size_t p = valueAt(s, 0, s.size(), key);
  if (p == std::string::npos || s[p] != '[') return false;
  const size_t end = matchClose(s, p);
  if (end == std::string::npos) return false;
  for (size_t i = p + 1; i < end; ++i) {
    if (s[i] != '{') continue;
    const size_t e = matchClose(s, i);
    if (e == std::string::npos || e > end) return false;
    fn(i, e);
    i = e - 1;
  }
  return true;
}

// --- channel files ----------------------------------------------------------------------

template <typename T>
bool readValues(const std::string& path, int perSample, std::vector<T>* out, std::string* err) {
  std::string raw;
  if (!readFile(path, &raw)) {
    *err = "missing " + path;
    return false;
  }
  if (raw.size() % (sizeof(T) * static_cast<size_t>(perSample)) != 0) {
    *err = "unexpected size of " + path;
    return false;
  }
  out->resize(raw.size() / sizeof(T));
  std::memcpy(out->data(), raw.data(), raw.size());  // little-endian on Mac and ESP32 alike
  return true;
}

}  // namespace

bool load(const std::string& dir, Session* s, std::string* err) {
  *s = Session();
  std::string d = dir;
  while (d.size() > 1 && d.back() == '/') d.pop_back();
  const size_t slash = d.find_last_of('/');
  s->name = slash == std::string::npos ? d : d.substr(slash + 1);

  std::string js;
  if (!readFile(d + "/session.json", &js)) {
    *err = "missing session.json";
    return false;
  }
  stringAt(js, 0, js.size(), "trackName", &s->trackName);
  eachObject(js, "laps", [&](size_t b, size_t e) {
    Lap l;
    int64_t v = 0;
    if (intAt(js, b, e, "number", &v)) l.number = static_cast<int>(v);
    intAt(js, b, e, "startTimestamp", &l.startMs);
    l.finished = intAt(js, b, e, "finishTimestamp", &l.finishMs);
    boolAt(js, b, e, "isInvalid", &l.invalid);
    s->laps.push_back(l);
  });

  std::string fr;
  if (!readFile(d + "/sessionfragment.json", &fr)) {
    *err = "missing sessionfragment.json";
    return false;
  }
  int64_t dev = -1;
  if (!intAt(fr, 0, fr.size(), "primaryGpsDeviceIndex", &dev)) {
    eachObject(fr, "items", [&](size_t b, size_t e) {
      int64_t type = 0;
      int64_t id = 0;
      if (dev < 0 && intAt(fr, b, e, "type", &type) && type == 1 && intAt(fr, b, e, "id", &id)) {
        dev = id;
      }
    });
  }
  if (dev < 0) {
    *err = "no GPS device in sessionfragment.json";
    return false;
  }
  s->gpsDevice = static_cast<int>(dev);

  const std::string base = d + "/channel_1_" + std::to_string(dev) + "_0_";
  std::vector<int64_t> t;
  std::vector<int32_t> pos, spd, alt, brg, sats;
  if (!readValues(base + "1_1", 1, &t, err) || !readValues(base + "3_1", 2, &pos, err) ||
      !readValues(base + "4_0", 1, &spd, err) || !readValues(base + "5_0", 1, &alt, err) ||
      !readValues(base + "6_0", 1, &brg, err)) {
    return false;
  }
  const size_t n = t.size();
  if (pos.size() != 2 * n || spd.size() != n || alt.size() != n || brg.size() != n) {
    *err = "GPS channels have different lengths";
    return false;
  }
  std::string ignore;
  if (!readValues(base + "30002_0", 1, &sats, &ignore) || sats.size() != n) sats.clear();

  s->tMs = t;
  s->latE7.resize(n);
  s->lonE7.resize(n);
  s->speedMps.resize(n);
  s->bearingDeg.resize(n);
  s->altM.resize(n);
  for (size_t i = 0; i < n; ++i) {
    // 1/6,000,000 deg -> 1e-7 deg
    s->latE7[i] = static_cast<int32_t>(std::llround(pos[2 * i] * 10.0 / 6.0));
    s->lonE7[i] = static_cast<int32_t>(std::llround(pos[2 * i + 1] * 10.0 / 6.0));
    s->speedMps[i] = static_cast<float>(spd[i]) * 0.001f;
    s->bearingDeg[i] = static_cast<float>(brg[i]) * 0.001f;
    s->altM[i] = static_cast<float>(alt[i]) * 0.001f;
  }
  s->sats.assign(sats.begin(), sats.end());
  return true;
}

}  // namespace rcz
