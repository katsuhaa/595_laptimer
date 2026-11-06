#include "geo.h"

#include <cmath>

namespace lt {

namespace {
constexpr float kDegToRadF = 0.017453292519943295f;
constexpr double kPi = 3.14159265358979323846;
}  // namespace

float length(Vec2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }

Vec2 headingUnit(float headingDeg) {
  const float r = headingDeg * kDegToRadF;
  return {std::sin(r), std::cos(r)};
}

float headingOf(Vec2 v) {
  float h = std::atan2(v.x, v.y) / kDegToRadF;
  if (h < 0.0f) h += 360.0f;
  if (h >= 360.0f) h -= 360.0f;
  return h;
}

void Plane::init(int32_t lat0E7, int32_t lon0E7) {
  lat0_ = lat0E7;
  lon0_ = lon0E7;
  // WGS84 radii of curvature at the origin. Double on purpose: runs once per course.
  const double a = 6378137.0;
  const double e2 = 6.69437999014e-3;
  const double phi = lat0E7 * 1e-7 * kPi / 180.0;
  const double s = std::sin(phi);
  const double w = 1.0 - e2 * s * s;
  const double meridian = a * (1.0 - e2) / (w * std::sqrt(w));
  const double primeVertical = a / std::sqrt(w);
  const double radPerE7 = 1e-7 * kPi / 180.0;
  ky_ = static_cast<float>(meridian * radPerE7);
  kx_ = static_cast<float>(primeVertical * std::cos(phi) * radPerE7);
}

Vec2 Plane::toLocal(int32_t latE7, int32_t lonE7) const {
  const int64_t dLat = static_cast<int64_t>(latE7) - lat0_;
  const int64_t dLon = static_cast<int64_t>(lonE7) - lon0_;
  return {static_cast<float>(dLon) * kx_, static_cast<float>(dLat) * ky_};
}

void Plane::toGlobal(Vec2 p, int32_t* latE7, int32_t* lonE7) const {
  *latE7 = lat0_ + static_cast<int32_t>(std::lround(p.y / ky_));
  *lonE7 = lon0_ + static_cast<int32_t>(std::lround(p.x / kx_));
}

}  // namespace lt
