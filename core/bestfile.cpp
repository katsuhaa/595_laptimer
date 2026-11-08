#include "bestfile.h"

#include <cmath>
#include <cstring>

namespace lt {
namespace bestfile {

namespace {

void put16(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}
uint32_t get16(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8); }
uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

bool validId(const char* id) {
  if (!id) return false;
  int n = 0;
  for (; id[n]; ++n) {
    if (n >= 31 || id[n] <= 0x20 || id[n] >= 0x7F) return false;
  }
  return n > 0;
}

bool fail(const char** why, const char* code) {
  if (why) *why = code;
  return false;
}

}  // namespace

uint32_t crc32(uint32_t crc, const uint8_t* p, int n) {
  static const uint32_t kNibble[16] = {0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
                                       0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
                                       0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
  uint32_t c = ~crc;
  for (int i = 0; i < n; ++i) {
    c ^= p[i];
    c = (c >> 4) ^ kNibble[c & 15u];
    c = (c >> 4) ^ kNibble[c & 15u];
  }
  return ~c;
}

int write(uint8_t* out, int cap, const char* courseId, uint32_t lapMs, int32_t originLatE7, int32_t originLonE7,
          const Point* pts, int n) {
  if (n < 0 || n > kMaxPoints || (n > 0 && !pts) || !validId(courseId)) return 0;
  const int size = fileBytes(n);
  if (cap < size) return 0;
  for (int i = 1; i < n; ++i) {
    if (pts[i].tMs < pts[i - 1].tMs) return 0;
  }
  std::memset(out, 0, kHeaderBytes);
  std::memcpy(out, "LTB1", 4);
  put16(out + 4, kVersion);
  put16(out + 6, kHeaderBytes);
  std::memcpy(out + 8, courseId, std::strlen(courseId));
  put32(out + 40, lapMs);
  put32(out + 44, static_cast<uint32_t>(originLatE7));
  put32(out + 48, static_cast<uint32_t>(originLonE7));
  put32(out + 52, static_cast<uint32_t>(n));
  uint8_t* p = out + kHeaderBytes;
  for (int i = 0; i < n; ++i, p += kPointBytes) {
    put32(p, static_cast<uint32_t>(pts[i].latE7));
    put32(p + 4, static_cast<uint32_t>(pts[i].lonE7));
    put32(p + 8, pts[i].tMs);
  }
  put32(p, crc32(0, out, size - 4));
  return size;
}

bool readInfo(const uint8_t* data, int len, Info* info, const char** why) {
  if (why) *why = "";
  if (!data || len < kHeaderBytes) return fail(why, "short");
  if (std::memcmp(data, "LTB1", 4) != 0) return fail(why, "bad-magic");
  const uint32_t version = get16(data + 4);
  if (version != kVersion) return fail(why, "bad-version");
  if (get16(data + 6) != static_cast<uint32_t>(kHeaderBytes)) return fail(why, "bad-header");
  Info in{};
  in.version = static_cast<int>(version);
  std::memcpy(in.courseId, data + 8, 32);
  if (in.courseId[31] != '\0' || !validId(in.courseId)) return fail(why, "bad-id");
  in.lapMs = get32(data + 40);
  in.originLatE7 = static_cast<int32_t>(get32(data + 44));
  in.originLonE7 = static_cast<int32_t>(get32(data + 48));
  const uint32_t n = get32(data + 52);
  if (n > static_cast<uint32_t>(kMaxPoints)) return fail(why, "too-many-points");
  in.numPoints = static_cast<int>(n);
  *info = in;
  return true;
}

bool read(const uint8_t* data, int len, Info* info, Point* pts, int cap, const char** why) {
  Info in{};
  if (!readInfo(data, len, &in, why)) return false;
  const int size = fileBytes(in.numPoints);
  if (len != size) return fail(why, "bad-size");
  if (get32(data + size - 4) != crc32(0, data, size - 4)) return fail(why, "bad-crc");
  if (in.numPoints > cap) return fail(why, "no-room");
  const uint8_t* p = data + kHeaderBytes;
  for (int i = 0; i < in.numPoints; ++i, p += kPointBytes) {
    pts[i].latE7 = static_cast<int32_t>(get32(p));
    pts[i].lonE7 = static_cast<int32_t>(get32(p + 4));
    pts[i].tMs = get32(p + 8);
    if (i > 0 && pts[i].tMs < pts[i - 1].tMs) return fail(why, "bad-time");
  }
  *info = in;
  return true;
}

int writeTrace(uint8_t* out, int cap, const char* courseId, uint32_t lapMs, const Plane& pl, const TracePoint* tp,
               int n) {
  if (n < 0 || n > kMaxPoints || cap < fileBytes(n) || (n > 0 && !tp) || !validId(courseId)) return 0;
  // the points go straight into their place in the file, then write() fills the rest in place
  uint8_t* p = out + kHeaderBytes;
  uint32_t prev = 0;
  for (int i = 0; i < n; ++i, p += kPointBytes) {
    int32_t lat = 0;
    int32_t lon = 0;
    pl.toGlobal({tp[i].x, tp[i].y}, &lat, &lon);
    const float ms = tp[i].tS * 1000.0f;
    uint32_t t = ms > 0.0f ? static_cast<uint32_t>(std::lround(ms)) : 0u;
    if (t < prev) t = prev;  // float rounding must not make the times go back
    prev = t;
    put32(p, static_cast<uint32_t>(lat));
    put32(p + 4, static_cast<uint32_t>(lon));
    put32(p + 8, t);
  }
  const int size = fileBytes(n);
  std::memset(out, 0, kHeaderBytes);
  std::memcpy(out, "LTB1", 4);
  put16(out + 4, kVersion);
  put16(out + 6, kHeaderBytes);
  std::memcpy(out + 8, courseId, std::strlen(courseId));
  put32(out + 40, lapMs);
  put32(out + 44, static_cast<uint32_t>(pl.lat0E7()));
  put32(out + 48, static_cast<uint32_t>(pl.lon0E7()));
  put32(out + 52, static_cast<uint32_t>(n));
  put32(out + size - 4, crc32(0, out, size - 4));
  return size;
}

int readTrace(const uint8_t* data, int len, const Plane& pl, TracePoint* out, int cap, Info* info,
              const char** why) {
  Info in{};
  if (!readInfo(data, len, &in, why)) return -1;
  const int size = fileBytes(in.numPoints);
  const char* code = nullptr;
  if (len != size) {
    code = "bad-size";
  } else if (get32(data + size - 4) != crc32(0, data, size - 4)) {
    code = "bad-crc";
  } else if (in.numPoints > cap) {
    code = "no-room";
  }
  const uint8_t* p = data + kHeaderBytes;
  uint32_t prev = 0;
  for (int i = 0; !code && i < in.numPoints; ++i, p += kPointBytes) {
    const uint32_t t = get32(p + 8);
    if (i > 0 && t < prev) {
      code = "bad-time";
      break;
    }
    prev = t;
    const Vec2 v = pl.toLocal(static_cast<int32_t>(get32(p)), static_cast<int32_t>(get32(p + 4)));
    out[i] = {v.x, v.y, static_cast<float>(t) / 1000.0f};
  }
  if (code) {
    fail(why, code);
    return -1;
  }
  if (info) *info = in;
  return in.numPoints;
}

}  // namespace bestfile
}  // namespace lt
