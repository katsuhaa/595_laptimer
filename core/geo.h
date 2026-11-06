// core/geo.h - local east/north plane around a course origin.
// Shared by the firmware and the host tests: no heap, float math only after init().
#pragma once

#include <cstdint>

namespace lt {

struct Vec2 {
  float x;  // east [m]
  float y;  // north [m]
};

inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(Vec2 a, float k) { return {a.x * k, a.y * k}; }
inline float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
float length(Vec2 a);

// Heading in degrees (0 = north, clockwise) to a unit vector, and back ([0, 360)).
Vec2 headingUnit(float headingDeg);
float headingOf(Vec2 v);

// Positions stay in integer 1e-7 degrees (UBX NAV-PVT units), so differences are
// exact; only the local offsets (a few km) become float metres.
class Plane {
 public:
  void init(int32_t lat0E7, int32_t lon0E7);
  Vec2 toLocal(int32_t latE7, int32_t lonE7) const;
  void toGlobal(Vec2 p, int32_t* latE7, int32_t* lonE7) const;
  int32_t lat0E7() const { return lat0_; }
  int32_t lon0E7() const { return lon0_; }

 private:
  int32_t lat0_ = 0;
  int32_t lon0_ = 0;
  float ky_ = 0.0f;  // metres per 1e-7 degree of latitude
  float kx_ = 0.0f;  // metres per 1e-7 degree of longitude at the origin
};

}  // namespace lt
