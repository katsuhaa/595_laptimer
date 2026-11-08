// Tests of core/imucal (IMU mounting-angle correction). Ported from the round gauge's
// pc_test/unit_main.cpp section 19 (katsuhaa/abarth595-round-gauge 05ab0a1), plus the gyro zero,
// the yaw rate and other default axes.
#include <cmath>
#include <cstring>

#include "core/imucal.h"
#include "tmini.h"

namespace {

bool near(float a, float b, float tol) { return std::fabs(a - b) < tol; }

// Sensor tilted 30 deg forward: car up = sensor (0, -sin30, cos30), car forward = (0, cos30, sin30),
// right = (1, 0, 0)
constexpr float s30 = 0.5f, c30 = 0.8660254f;
void sensor(float lon, float lat, float vert, float* x, float* y, float* z) {  // car [g] -> sensor
  *x = lat;
  *y = lon * c30 - vert * s30;
  *z = lon * s30 + vert * c30;
}

struct Drive {
  lt::ImuCal& imu;
  uint32_t t = 100000;
  float x = 0, y = 0, z = 0;
  void still(float tiltLon, int samples) {  // stopped (on a slope: tiltLon)
    for (int i = 0; i < samples; i++) {
      sensor(tiltLon, 0, std::sqrt(1.0f - tiltLon * tiltLon), &x, &y, &z);
      imu.sample(x, y, z, true, 0.0f, t);
      t += 20;
    }
  }
  void launch(float aLon, float aLat, float kmhEnd) {  // 3 km/h up to kmhEnd, +2.1 km/h per 0.2 s
    float kmh = 3.0f;
    for (int i = 0; kmh <= kmhEnd; i++) {
      sensor(aLon, aLat, 1.0f, &x, &y, &z);
      imu.sample(x, y, z, true, kmh, t);
      t += 20;
      if (i % 10 == 9) kmh += 2.1f;  // about 0.3 g
    }
  }
};

}  // namespace

TEST(imucal_learn_up_forward_like_round_gauge) {
  lt::ImuCal imu;
  Drive dv{imu};
  float x, y, z, lon, lat, vert;
  dv.still(0, 175);  // 3.5 s
  CHECK(imu.levelOk() && !imu.forwardOk() && imu.calVersion() == 1);
  sensor(0, 0, 1.0f, &x, &y, &z);
  imu.sample(x, y, z, true, 0.0f, dv.t);
  imu.car(&lon, &lat, &vert);
  CHECK(std::fabs(vert) < 0.01f);  // gravity removed
  dv.launch(0.3f, 0, 10.0f);  // up to 10 km/h only (like reversing): not learned
  CHECK(!imu.forwardOk());
  dv.still(0, 20);
  dv.launch(0.3f, 0, 25.0f);
  CHECK(imu.forwardOk() && imu.calVersion() == 2);
  sensor(0.3f, 0, 1.0f, &x, &y, &z);
  imu.sample(x, y, z, true, 30.0f, dv.t);
  imu.car(&lon, &lat, &vert);
  CHECKF(near(lon, 0.3f, 0.02f) && near(lat, 0, 0.02f) && near(vert, 0, 0.02f), "%.3f %.3f %.3f",
         static_cast<double>(lon), static_cast<double>(lat), static_cast<double>(vert));
  sensor(0, 0.4f, 1.0f, &x, &y, &z);
  imu.sample(x, y, z, true, 30.0f, dv.t += 20);
  imu.car(&lon, &lat, &vert);
  CHECK(near(lon, 0, 0.02f) && near(lat, 0.4f * static_cast<float>(lt::IMU_LAT_SIGN), 0.02f));
  float gl, gt, gv;
  imu.toCar(0, -s30 * 10.0f, c30 * 10.0f, &gl, &gt, &gv);  // 10 deg/s about the car's up
  CHECK(near(gv, 10.0f, 0.05f) && near(gl, 0, 0.05f));
  // later launches move "forward" a little only: a launch turning left at 0.1 g (about 18 deg)
  dv.still(0, 20);
  dv.launch(0.3f, -0.1f, 25.0f);
  float cal[9], cal2[9];
  imu.exportCal(cal);
  const float fdot = cal[4] * c30 + cal[5] * s30;
  CHECK(imu.forwardOk() && fdot > 0.995f);
  // export / import
  CHECK(cal[6] == 1.0f && cal[7] == 1.0f && near(cal[1], -s30, 0.01f) && near(cal[2], c30, 0.01f));
  imu.reset();
  CHECK(!imu.levelOk() && !imu.forwardOk());
  imu.importCal(cal);
  imu.exportCal(cal2);
  CHECK(imu.levelOk() && imu.forwardOk() && std::memcmp(cal, cal2, sizeof(float) * 9) == 0);
  const float broken[9] = {0, 0, 0, 0, 0, 0, 1, 1, 0};
  imu.reset();
  imu.importCal(broken);
  CHECK(!imu.levelOk() && !imu.forwardOk() && imu.gravity() == 1.0f);
  imu.importCal(cal);
  // stopping on a slope (15 deg) does not change "up"
  const uint32_t ver = imu.calVersion();
  dv.still(-0.2588f, 175 * 3);
  imu.exportCal(cal2);
  CHECK(imu.calVersion() == ver && imu.forwardOk() && near(cal2[2], c30, 0.005f));
}

TEST(imucal_long_term_up_and_remount) {
  lt::ImuCal imu;
  Drive dv{imu};
  float cal[9], cal2[9];
  // learned on a slope (15 deg off), corrected by 30 min of flat driving (3 time constants)
  dv.still(-0.2588f, 175);
  imu.exportCal(cal2);
  CHECK(imu.levelOk() && std::fabs(cal2[2] - c30) > 0.03f);
  for (int i = 0; i < 50 * 60 * 30; i++) {
    sensor(0, 0, 1.0f, &dv.x, &dv.y, &dv.z);
    imu.sample(dv.x, dv.y, dv.z, true, 30.0f, dv.t);
    dv.t += 20;
  }
  imu.exportCal(cal2);
  CHECK(near(cal2[1], -s30, 0.02f) && near(cal2[2], c30, 0.02f) && imu.calVersion() > 1);
  // a good calibration, then 60 deg different stops 5 times: re-learn "up", forget "forward"
  imu.reset();
  dv.still(0, 175);
  dv.still(0, 20);
  dv.launch(0.3f, 0, 25.0f);
  CHECK(imu.levelOk() && imu.forwardOk());
  imu.exportCal(cal);
  uint32_t t = dv.t;
  const uint32_t ver = imu.calVersion();
  for (int k = 0; k < lt::IMU_RELEARN_COUNT - 1; k++) {
    for (int i = 0; i < 175; i++) {
      imu.sample(0.866f, 0, 0.5f, true, 0.0f, t);
      t += 20;
    }
    imu.sample(0, 0, 1.0f, true, 10.0f, t += 20);  // moves a little between the stops
    imu.sample(0, 0, 1.0f, true, 10.0f, t += 20);
  }
  CHECK(imu.calVersion() == ver && imu.forwardOk());
  for (int i = 0; i < 175; i++) {
    imu.sample(0.866f, 0, 0.5f, true, 0.0f, t);
    t += 20;
  }
  imu.exportCal(cal2);
  CHECK(imu.calVersion() == ver + 1 && imu.levelOk() && !imu.forwardOk() && near(cal2[0], 0.866f, 0.02f));
}

TEST(imucal_desk_without_speed) {
  lt::ImuCal imu;  // defaults: +X up, +Z (back) forward
  float lon, lat, vert, cal2[9];
  uint32_t t = 500000;
  imu.sample(1.0f, 0, 0, false, 0.0f, t += 20);
  imu.car(&lon, &lat, &vert);
  CHECK(near(lon, 0, 0.001f) && near(lat, 0, 0.001f) && near(vert, 0, 0.001f));
  imu.sample(0, 0, 1.0f, false, 0.0f, t += 20);  // face down (back up)
  imu.car(&lon, &lat, &vert);
  CHECK(near(lon, 1.0f, 0.001f) && near(lat, 0, 0.001f) && near(vert, -1.0f, 0.001f));
  imu.sample(0, 0, -1.0f, false, 0.0f, t += 20);  // face up
  imu.car(&lon, &lat, &vert);
  CHECK(near(lon, -1.0f, 0.001f) && near(lat, 0, 0.001f) && near(vert, -1.0f, 0.001f));
  imu.sample(0, -1.0f, 0, false, 0.0f, t += 20);
  imu.car(&lon, &lat, &vert);
  CHECK(near(lat, -1.0f * static_cast<float>(lt::IMU_LAT_SIGN), 0.001f) && near(lon, 0, 0.001f) &&
        near(vert, -1.0f, 0.001f));
  for (int i = 0; i < 300; i++) {  // shaking (0.08 g between samples): not still
    const float j = (i & 1) ? 0.04f : -0.04f;
    imu.sample(1.0f, j, 0, false, 0.0f, t);
    t += 20;
  }
  CHECK(!imu.levelOk() && !imu.stillNow());
  // standing, tilted 20 deg back, sensor scale 1.1: after 3 s "up" and gravity 1.1 are learned
  const float c20 = 0.9396926f, s20 = 0.3420201f;
  for (int i = 0; i < 175; i++) {
    imu.sample(1.1f * c20, 0, 1.1f * s20, false, 0.0f, t);
    t += 20;
  }
  imu.exportCal(cal2);
  imu.car(&lon, &lat, &vert);
  CHECK(imu.levelOk() && !imu.forwardOk() && imu.stillNow() && near(cal2[0], c20, 0.01f) && near(cal2[2], s20, 0.01f));
  CHECK(near(imu.gravity(), 1.1f, 0.005f) && near(vert, 0, 0.005f) && near(lon, 0, 0.005f) && near(cal2[8], 1.1f, 0.005f));
  for (int i = 0; i < 175; i++) {
    imu.sample(1.1f * c20, 0, 1.1f * s20, false, 0.0f, t);
    t += 20;
  }
  CHECK(imu.stillNow());  // across the 3 s windows
  imu.sample(1.1f * c20, 0.11f, 1.1f * s20, false, 0.0f, t += 20);
  imu.car(&lon, &lat, &vert);
  CHECK(near(lat, 0.1f * static_cast<float>(lt::IMU_LAT_SIGN), 0.005f) && near(vert, 0, 0.005f) && near(lon, 0, 0.005f));
  for (int i = 0; i < 180; i++) {  // stood up again: follows the placed direction
    imu.sample(1.1f, 0, 0, false, 0.0f, t);
    t += 20;
  }
  imu.exportCal(cal2);
  imu.car(&lon, &lat, &vert);
  CHECK(near(cal2[0], 1.0f, 0.01f) && near(vert, 0, 0.005f) && near(lon, 0, 0.005f));
  const uint32_t v0 = imu.calVersion();
  for (int i = 0; i < 300; i++) {  // face down (90 deg from the default up): "up" stays
    imu.sample(0, 0, 1.1f, false, 0.0f, t);
    t += 20;
  }
  imu.exportCal(cal2);
  imu.car(&lon, &lat, &vert);
  CHECK(imu.calVersion() == v0 && near(cal2[0], 1.0f, 0.01f) && !imu.stillNow() && near(lon, 1.0f, 0.01f) &&
        near(vert, -1.0f, 0.01f));
  for (int i = 0; i < 60; i++) {  // moved by hand
    imu.sample(1.1f + 0.3f * ((i & 1) ? 1.0f : -1.0f), 0, 0, false, 0.0f, t);
    t += 20;
  }
  CHECK(!imu.stillNow() && imu.levelOk());
}

TEST(imucal_gyro_bias_and_yaw) {
  lt::ImuCal imu;  // +X up
  uint32_t t = 1000;
  const float bias[3] = {0.8f, -0.3f, 0.2f};  // deg/s
  // moving: the zero is not learned
  for (int i = 0; i < 50; i++) {
    imu.sample(1.0f + ((i & 1) ? 0.1f : -0.1f), 0, 0, false, 0.0f, t += 20);
    imu.updateGyroBias(bias[0], bias[1], bias[2]);
  }
  CHECK(!imu.gyroBiasOk());
  // still for 1 s or more: follows the gyro (time constant 1 s at 50 Hz)
  for (int i = 0; i < 50 * 8; i++) {
    imu.sample(1.0f, 0, 0, false, 0.0f, t += 20);
    imu.updateGyroBias(bias[0], bias[1], bias[2]);
  }
  float b[3];
  imu.gyroBias(b);
  CHECKF(imu.gyroBiasOk() && near(b[0], bias[0], 0.01f) && near(b[1], bias[1], 0.01f) && near(b[2], bias[2], 0.01f),
         "bias %.3f %.3f %.3f", static_cast<double>(b[0]), static_cast<double>(b[1]), static_cast<double>(b[2]));
  // yaw = rotation about "up" (+X here) minus the zero
  CHECK(near(imu.yawDps(bias[0] + 25.0f, bias[1], bias[2]), 25.0f, 0.05f));
  CHECK(near(imu.yawDps(bias[0], bias[1] + 25.0f, bias[2]), 0.0f, 0.05f));
}

TEST(imucal_other_default_axes) {
  // e.g. if F1 shows +Y up and -Z forward on the 2.41 board
  const float up[3] = {0.0f, 1.0f, 0.0f}, fwd[3] = {0.0f, 0.0f, -1.0f};
  lt::ImuCal imu(up, fwd);
  float lon, lat, vert;
  imu.sample(0, 1.0f, 0, false, 0.0f, 100);
  imu.car(&lon, &lat, &vert);
  CHECK(near(lon, 0, 0.001f) && near(lat, 0, 0.001f) && near(vert, 0, 0.001f));
  imu.sample(0.2f, 1.0f, -0.3f, false, 0.0f, 120);  // 0.3 g forward, 0.2 g on +X
  imu.car(&lon, &lat, &vert);
  CHECK(near(lon, 0.3f, 0.001f) && near(std::fabs(lat), 0.2f, 0.001f) && near(vert, 0, 0.001f));
  float l2, t2, v2;
  imu.carOf(0.2f, 1.0f, -0.3f, &l2, &t2, &v2);  // carOf = car() for any sample
  CHECK(l2 == lon && t2 == lat && v2 == vert);
  imu.carOf(0.0f, 1.1f, 0.0f, &l2, &t2, &v2);  // gravity removed, not scaled before learning
  CHECK(near(v2, 0.1f, 0.001f) && near(l2, 0, 0.001f));
  imu.begin();
  float cal[9];
  imu.exportCal(cal);
  CHECK(cal[1] == 1.0f && cal[5] == -1.0f && cal[6] == 0.0f);  // defaults kept by begin()
}
