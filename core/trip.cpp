#include "trip.h"

namespace lt {

void Trip::update(const VehValues& v, uint32_t nowMs, int boundaryRpm) {
  uint32_t dt = started_ ? nowMs - lastMs_ : 0;
  started_ = true;
  lastMs_ = nowMs;
  if (dt > 500) dt = 500;  // a gap (screen change etc.) does not count too much
  if (v.speedOk && v.speedKmh >= TRIP_MOVING_KMH) {
    moving_ += dt;
    if (v.throttleOk && v.throttlePct >= TRIP_WOT_PCT) wot_ += dt;
  }
  if (v.rpmOk) {  // standing still or moving
    if (v.rpm >= boundaryRpm) above_ += dt;  // round gauge 2.12.0: at or above, as the overload time
    else below_ += dt;
  }
}

}  // namespace lt
