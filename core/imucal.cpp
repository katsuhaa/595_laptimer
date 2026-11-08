#include "imucal.h"

#include <cmath>

namespace lt {

namespace {

constexpr float NEAR_UP = 0.9f;      // within 25 deg of the learned "up" = same
constexpr float NEAR_FWD = 0.7f;     // within 45 deg of the learned "forward"
constexpr float VER_UP = 0.99939f;   // moved 2 deg or more: new version (save)
constexpr float VER_FWD = 0.99905f;  // 2.5 deg

}  // namespace

// vector helpers (same as the round gauge)
namespace {
using V3 = ImuCal::V3;
inline V3 add(V3 a, V3 b) { return V3{a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 sub(V3 a, V3 b) { return V3{a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 mul(V3 a, float k) { return V3{a.x * k, a.y * k, a.z * k}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float norm(V3 a) { return std::sqrt(dot(a, a)); }
}  // namespace

ImuCal::ImuCal(const float defUp[3], const float defFwd[3])
    : defUp_{defUp[0], defUp[1], defUp[2]}, defFwd_{defFwd[0], defFwd[1], defFwd[2]} {
  begin();
}

ImuCal::V3 ImuCal::unit(V3 a) const {
  const float n = norm(a);
  return n > 1e-6f ? mul(a, 1.0f / n) : defUp_;
}

// Part of v perpendicular to up. When v is parallel to up, the default forward / up stand in.
ImuCal::V3 ImuCal::horiz(V3 v, V3 up) const {
  V3 h = sub(v, mul(up, dot(v, up)));
  if (norm(h) < 1e-3f) h = sub(defFwd_, mul(up, dot(defFwd_, up)));
  if (norm(h) < 1e-3f) h = sub(defUp_, mul(up, dot(defUp_, up)));
  return unit(h);
}

void ImuCal::begin() {
  up_ = defUp_;
  fwd_ = defFwd_;
  levelOk_ = fwdOk_ = false;
  calVer_ = 0;
  upAtVer_ = up_;
  fwdAtVer_ = fwd_;
  g0_ = 1.0f;
  upReject_ = fwdReject_ = 0;
  stillSince_ = 0;
  last_ = V3{0.0f, 0.0f, 0.0f};
  lastMs_ = 0;
  lt_ = up_;
  stillSum_ = V3{0.0f, 0.0f, 0.0f};
  stillN_ = 0;
  stillStart_ = 0;
  still_ = false;
  fwdSum_ = V3{0.0f, 0.0f, 0.0f};
  fwdN_ = 0;
  fwdDone_ = false;
  accel_ = false;
  prevKmh_ = 0.0f;
  prevKmhMs_ = 0;
  bias_[0] = bias_[1] = bias_[2] = 0.0f;
  biasOk_ = false;
}

void ImuCal::setUp(V3 up, bool forgetFwd) {
  up_ = up;
  lt_ = up;
  upAtVer_ = up;
  levelOk_ = true;
  upReject_ = 0;
  calVer_++;
  if (forgetFwd) {
    fwdOk_ = false;
    fwdReject_ = 0;
    fwdSum_ = V3{0.0f, 0.0f, 0.0f};
    fwdN_ = 0;
  }
}

// No speed (on the desk): the placed direction becomes "up"; the version moves only for 2 deg or more
void ImuCal::refreshUp(V3 up) {
  up_ = up;
  lt_ = up;
  upReject_ = 0;
  if (dot(up, upAtVer_) < VER_UP) {
    upAtVer_ = up;
    calVer_++;
  }
}

void ImuCal::setGravity(float m) {
  if (m < 0.8f || m > 1.2f) return;
  if (std::fabs(m - g0_) > 0.02f) calVer_++;
  g0_ = m;
}

void ImuCal::sample(float ax, float ay, float az, bool speedOk, float speedKmh, uint32_t nowMs) {
  const V3 a = {ax, ay, az};
  const float jump = norm(sub(a, last_));  // change from the previous sample
  last_ = a;
  uint32_t dt = lastMs_ ? nowMs - lastMs_ : 20;
  if (dt > 100) dt = 100;
  lastMs_ = nowMs;
  const float n = norm(a);
  const bool plausible = n > 0.8f && n < 1.2f;  // near 1 g only (no strong vibration, not in the hand)
  // Stopped: by speed when there is one; else "not moving, about 1 g, standing (45 deg of default up)"
  const bool quiet = jump < IMU_QUIET_G && plausible && dot(a, defUp_) > n * IMU_QUIET_TILT_COS;
  const bool stopped = speedOk ? (speedKmh < IMU_STILL_KMH) : quiet;
  if (!speedOk) fwdN_ = 0;  // "forward" needs the speed
  // up, long term: average while driving, time constant IMU_UP_TAU_MS
  if (levelOk_ && plausible && speedKmh >= IMU_UP_EMA_MIN_KMH) {
    const float k = static_cast<float>(dt) / static_cast<float>(IMU_UP_TAU_MS);
    lt_ = add(lt_, mul(sub(a, lt_), k));
    up_ = unit(lt_);
    if (dot(up_, upAtVer_) < VER_UP) {
      upAtVer_ = up_;
      calVer_++;
    }
  }
  // up, short term: average while stopped
  if (stopped) {
    if (!still_) {
      still_ = true;
      stillSince_ = nowMs;
      stillStart_ = nowMs;
      stillSum_ = V3{0.0f, 0.0f, 0.0f};
      stillN_ = 0;
    }
    if (plausible) {
      stillSum_ = add(stillSum_, a);
      stillN_++;
    }
    if (static_cast<uint32_t>(nowMs - stillStart_) >= IMU_LEVEL_STILL_MS && stillN_ >= 20) {
      const V3 mean = mul(stillSum_, 1.0f / static_cast<float>(stillN_));
      const V3 up = unit(mean);
      setGravity(norm(mean));  // gravity magnitude (scale error) too
      if (!levelOk_) {
        setUp(up, false);  // first time (a slope is corrected by the long-term average later)
      } else if (!speedOk) {
        refreshUp(up);  // on the desk: follow the placed direction ("forward" stays)
      } else if (dot(up, up_) > NEAR_UP) {
        upReject_ = 0;
      } else if (++upReject_ >= IMU_RELEARN_COUNT) {
        setUp(up, true);  // very different several times = re-mounted: learn "forward" again too
      }
      stillStart_ = nowMs;  // next window
      stillSum_ = V3{0.0f, 0.0f, 0.0f};
      stillN_ = 0;
    }
  } else {
    still_ = false;
  }
  // forward: horizontal acceleration while the speed rises from IMU_FWD_FROM_KMH; averaged when it
  // reaches IMU_FWD_DONE_KMH (that fast is forward; reversing usually is not)
  if (levelOk_ && speedOk) {
    const uint32_t sdt = nowMs - prevKmhMs_;
    if (sdt >= 200) {
      accel_ = (speedKmh - prevKmh_) > 0.5f;  // +0.5 km/h in 0.2 s (about 0.07 g)
      prevKmh_ = speedKmh;
      prevKmhMs_ = nowMs;
    }
    if (speedKmh < IMU_FWD_FROM_KMH) {
      fwdSum_ = V3{0.0f, 0.0f, 0.0f};
      fwdN_ = 0;
      fwdDone_ = false;
    } else if (speedKmh > IMU_FWD_TO_KMH || !accel_) {
      fwdSum_ = V3{0.0f, 0.0f, 0.0f};
      fwdN_ = 0;
    } else if (!fwdDone_) {
      const V3 h = sub(a, mul(up_, dot(a, up_)));  // horizontal part
      if (norm(h) > 0.08f) {
        fwdSum_ = add(fwdSum_, h);
        fwdN_++;
      }
      if (speedKmh >= IMU_FWD_DONE_KMH && fwdN_ >= IMU_FWD_SAMPLES) {
        const V3 f = unit(mul(fwdSum_, 1.0f / static_cast<float>(fwdN_)));
        if (!fwdOk_) {
          fwd_ = f;
          fwdAtVer_ = f;
          fwdOk_ = true;
          fwdReject_ = 0;
          calVer_++;
        } else if (dot(f, fwd_) > NEAR_FWD) {
          fwd_ = unit(add(mul(fwd_, 0.75f), mul(f, 0.25f)));  // move a little each time
          fwdReject_ = 0;
          if (dot(fwd_, fwdAtVer_) < VER_FWD) {
            fwdAtVer_ = fwd_;
            calVer_++;
          }
        } else if (++fwdReject_ >= IMU_RELEARN_COUNT) {
          fwd_ = f;
          fwdAtVer_ = f;
          fwdReject_ = 0;
          calVer_++;
        }
        fwdSum_ = V3{0.0f, 0.0f, 0.0f};
        fwdN_ = 0;
        fwdDone_ = true;
      }
    }
  }
}

bool ImuCal::stillNow() const { return still_ && static_cast<uint32_t>(lastMs_ - stillSince_) >= 1000; }

void ImuCal::toCar(float x, float y, float z, float* lon, float* lat, float* vert) const {
  const V3 a = {x, y, z};
  const V3 up = up_;
  const V3 fwd = horiz(fwd_, up);  // forward made horizontal
  const V3 right = cross(fwd, up);
  *lon = dot(a, fwd);
  *lat = dot(a, right) * static_cast<float>(IMU_LAT_SIGN);
  *vert = dot(a, up);
}

void ImuCal::car(float* lon, float* lat, float* vert) const { carOf(last_.x, last_.y, last_.z, lon, lat, vert); }

void ImuCal::carOf(float ax, float ay, float az, float* lon, float* lat, float* vert) const {
  toCar(ax, ay, az, lon, lat, vert);
  const float k = 1.0f / g0_;  // scale error (1 g while stopped)
  *lon *= k;
  *lat *= k;
  *vert = *vert * k - 1.0f;  // gravity removed
}

void ImuCal::exportCal(float out[9]) const {
  out[0] = up_.x;
  out[1] = up_.y;
  out[2] = up_.z;
  out[3] = fwd_.x;
  out[4] = fwd_.y;
  out[5] = fwd_.z;
  out[6] = levelOk_ ? 1.0f : 0.0f;
  out[7] = fwdOk_ ? 1.0f : 0.0f;
  out[8] = g0_;
}

void ImuCal::importCal(const float in[9]) {
  const V3 up = {in[0], in[1], in[2]}, fwd = {in[3], in[4], in[5]};
  const float nu = norm(up), nf = norm(fwd);
  if (in[6] > 0.5f && nu > 0.9f && nu < 1.1f) {
    up_ = up;  // as saved (length near 1)
    lt_ = up;
    upAtVer_ = up;
    levelOk_ = true;
    upReject_ = 0;
  }
  if (in[7] > 0.5f && nf > 0.9f && nf < 1.1f) {
    fwd_ = fwd;
    fwdAtVer_ = fwd;
    fwdOk_ = true;
    fwdReject_ = 0;
  }
  g0_ = (in[8] > 0.8f && in[8] < 1.2f) ? in[8] : 1.0f;
}

// round gauge imuPoll: while still, the zero follows the gyro (time constant 1 s at 50 Hz)
void ImuCal::updateGyroBias(float gx, float gy, float gz) {
  if (!stillNow()) return;
  bias_[0] += (gx - bias_[0]) * IMU_GYRO_BIAS_K;
  bias_[1] += (gy - bias_[1]) * IMU_GYRO_BIAS_K;
  bias_[2] += (gz - bias_[2]) * IMU_GYRO_BIAS_K;
  biasOk_ = true;
}

void ImuCal::gyroBias(float out[3]) const {
  out[0] = bias_[0];
  out[1] = bias_[1];
  out[2] = bias_[2];
}

float ImuCal::yawDps(float gx, float gy, float gz) const {
  float lon, lat, vert;
  toCar(gx - bias_[0], gy - bias_[1], gz - bias_[2], &lon, &lat, &vert);
  return vert;  // rotation about "up" (left turn positive)
}

}  // namespace lt
