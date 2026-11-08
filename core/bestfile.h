// core/bestfile.h - the ghost reference lap /best/<cid>.bin (UI設計 §4.4, ARCH §5.1).
// Binary, little-endian:
//   0   4    magic "LTB1"
//   4   u16  format version (1)
//   6   u16  header size (56)
//   8   32   course id, ASCII, NUL padded
//   40  u32  lap time [ms]
//   44  i32  origin latitude [1e-7 deg] (local plane the trace was recorded in)
//   48  i32  origin longitude [1e-7 deg]
//   52  u32  number of points N
//   56  N x {i32 latE7, i32 lonE7, u32 tMs}   tMs = since the lap started, not decreasing
//   56 + 12 N   u32 CRC-32 (the zip one, as in webcheck) of every byte before it
// Points are absolute positions, so a trace still fits after the course plane changes.
#pragma once

#include <cstdint>

#include "geo.h"
#include "ghost.h"

namespace lt {
namespace bestfile {

constexpr uint16_t kVersion = 1;
constexpr int kHeaderBytes = 56;
constexpr int kPointBytes = 12;
constexpr int kMaxPoints = 30000;  // 20 min at 25 Hz
constexpr int fileBytes(int numPoints) { return kHeaderBytes + numPoints * kPointBytes + 4; }

struct Point {
  int32_t latE7;
  int32_t lonE7;
  uint32_t tMs;
};

struct Info {
  int version;
  char courseId[32];
  uint32_t lapMs;
  int32_t originLatE7;
  int32_t originLonE7;
  int numPoints;
};

// Bytes written (fileBytes(n)), or 0: cap too small, bad id (ASCII 1..31 chars), n out of range or
// times decreasing.
int write(uint8_t* out, int cap, const char* courseId, uint32_t lapMs, int32_t originLatE7,
          int32_t originLonE7, const Point* pts, int n);
// Header only (no CRC check): to size the buffers before reading. why (may be null):
// short, bad-magic, bad-version, bad-header, bad-id, too-many-points.
bool readInfo(const uint8_t* data, int len, Info* info, const char** why);
// Everything, checked (also bad-size, bad-crc, bad-time, no-room when cap < N)
bool read(const uint8_t* data, int len, Info* info, Point* pts, int cap, const char** why);

// The same with the ghost's trace (lt::Ghost::bestTrace(), local plane metres and seconds).
// writeTrace converts with pl (its origin goes into the file), bytes written or 0 as write().
// readTrace converts into pl (the current course plane) with the checks of read(): the number of
// points, or -1 on an error (why).
int writeTrace(uint8_t* out, int cap, const char* courseId, uint32_t lapMs, const Plane& pl,
               const TracePoint* tp, int n);
int readTrace(const uint8_t* data, int len, const Plane& pl, TracePoint* out, int cap, Info* info,
              const char** why);

uint32_t crc32(uint32_t crc, const uint8_t* p, int n);  // crc32(0, "123456789", 9) = 0xCBF43926

}  // namespace bestfile
}  // namespace lt
