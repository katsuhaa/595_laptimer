#include <cmath>

#include "common.h"
#include "core/geo.h"
#include "tmini.h"

using tcommon::kLat0;
using tcommon::kLon0;

TEST(geo_round_trip_within_2cm) {
  const lt::Plane pl = tcommon::plane();
  double worst = 0.0;
  for (int i = -12; i <= 12; ++i) {
    for (int j = -12; j <= 12; ++j) {
      const lt::Vec2 p = {250.0f * static_cast<float>(i), 250.0f * static_cast<float>(j)};
      int32_t lat = 0;
      int32_t lon = 0;
      pl.toGlobal(p, &lat, &lon);
      const lt::Vec2 q = pl.toLocal(lat, lon);
      worst = std::fmax(worst, static_cast<double>(lt::length(q - p)));
    }
  }
  tmini::info("worst round trip error over +-3 km: %.4f m", worst);
  CHECKF(worst <= 0.02, "round trip error %.4f m", worst);
}

TEST(geo_scale_near_sodegaura) {
  // 0.001 deg near 35.39 N: about 110.95 m north-south, 90.85 m east-west (WGS84)
  const lt::Plane pl = tcommon::plane();
  const double north = static_cast<double>(pl.toLocal(kLat0 + 10000, kLon0).y);
  const double east = static_cast<double>(pl.toLocal(kLat0, kLon0 + 10000).x);
  tmini::info("0.001 deg: north %.3f m, east %.3f m", north, east);
  CHECKF(north > 110.90 && north < 111.00, "north %.3f", north);
  CHECKF(east > 90.70 && east < 91.00, "east %.3f", east);
}

TEST(geo_heading_conversions) {
  const lt::Vec2 e = lt::headingUnit(90.0f);
  CHECK(std::fabs(e.x - 1.0f) < 1e-6f && std::fabs(e.y) < 1e-6f);
  CHECK(std::fabs(lt::headingOf({0.0f, -1.0f}) - 180.0f) < 1e-4f);
  CHECK(std::fabs(lt::headingOf({-1.0f, 0.0f}) - 270.0f) < 1e-4f);
  CHECK(std::fabs(lt::headingOf({0.0f, 1.0f})) < 1e-4f);
}
