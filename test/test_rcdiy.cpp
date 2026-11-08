// Tests of core/rcdiy (RaceChrono DIY payloads and PID filter). Ported from the round gauge's
// pc_test/unit_main.cpp sections 17 and 18 (katsuhaa/abarth595-round-gauge 05ab0a1), plus the IMU
// payloads and the GPS sync/time schedule of meter_display.ino.
#include <cstring>

#include "core/rcdiy.h"
#include "core/ubx.h"
#include "tmini.h"

namespace {

using lt::rcdiy::RC_CAN_MIN_MS;

lt::GnssFix tokyoFix() {
  lt::GnssFix f{};
  f.fixOk = true;
  f.fixType = 3;
  f.numSV = 11;
  f.timeOk = true;
  f.year = 2026;
  f.month = 10;
  f.day = 14;
  f.hour = 3;
  f.min = 15;
  f.sec = 30;
  f.nano = 250000000;
  f.lat = 355812345;
  f.lon = 1396821234;
  f.hMSL = 45000;
  f.gSpeed = 13417;
  f.headMot = 12345000;
  f.pDOP = 150;
  return f;
}

int u16be(const uint8_t* p) { return (p[0] << 8) | p[1]; }
int16_t i16le(const uint8_t* p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }

}  // namespace

TEST(rcdiy_gps_main_and_time) {
  lt::GnssFix f = tokyoFix();
  uint8_t g[20];
  lt::rcdiy::encodeGpsMain(f, 5, g);
  // the formula of the specification written out
  const uint32_t tt = 15 * 30000 + 30 * 500 + 250 / 2;  // 465125
  CHECK(g[0] == ((5 << 5) | ((tt >> 16) & 0x1F)) && g[1] == ((tt >> 8) & 0xFF) && g[2] == (tt & 0xFF));
  CHECK(g[3] == ((1 << 6) | 11));  // fix 1 (GPS), 11 satellites
  CHECK(g[4] == 0x15 && g[5] == 0x35 && g[6] == 0x43 && g[7] == 0xF9);  // 355812345 big endian
  CHECK(g[8] == 0x53 && g[9] == 0x41 && g[10] == 0xCC && g[11] == 0xF2);
  CHECK(u16be(g + 12) == (45 + 500) * 10);  // (m + 500) * 10
  CHECK(u16be(g + 14) == 4830);             // 48.30 km/h * 100
  CHECK(u16be(g + 16) == 12345);            // 123.45 deg * 100
  CHECK(g[18] == 15 && g[19] == 0xFF);      // pDOP 1.5 * 10, no VDOP
  f.diffSoln = true;
  lt::rcdiy::encodeGpsMain(f, 0, g);
  CHECK(g[3] == ((2 << 6) | 11));
  f.diffSoln = false;
  f.hMSL = 3000000;  // 3000 m
  f.gSpeed = 100000;  // 360 km/h
  lt::rcdiy::encodeGpsMain(f, 0, g);
  CHECK(u16be(g + 12) == ((3000 + 500) | 0x8000) && u16be(g + 14) == (3600 | 0x8000));  // coarse steps
  f.hMSL = 45000;
  f.gSpeed = 13417;
  f.headMot = -1000000;  // -10 deg
  lt::rcdiy::encodeGpsMain(f, 0, g);
  CHECK(u16be(g + 16) == 35000);
  f.fixOk = false;
  lt::rcdiy::encodeGpsMain(f, 7, g);
  CHECK(g[0] >> 5 == 7 && g[3] == 11 && g[4] == 0x7F && g[5] == 0xFF && g[6] == 0xFF && g[7] == 0xFF &&
        g[8] == 0x7F && g[9] == 0xFF && g[12] == 0xFF && g[13] == 0xFF && g[14] == 0xFF && g[16] == 0xFF &&
        g[18] == 0xFF);  // no fix: "invalid" values
  f.fixOk = true;
  f.numSV = 70;
  lt::rcdiy::encodeGpsMain(f, 0, g);
  CHECK((g[3] & 0x3F) == 0x3E);  // satellites capped at 62
  f.numSV = 11;
  uint8_t tm[3];
  lt::rcdiy::encodeGpsTime(f, 5, tm);
  const uint32_t code = (2026 - 2000) * 8928 + (10 - 1) * 744 + (14 - 1) * 24 + 3;  // 239139
  CHECK(lt::rcdiy::dateHourCode(f) == code && tm[0] == ((5 << 5) | ((code >> 16) & 0x1F)) &&
        tm[1] == ((code >> 8) & 0xFF) && tm[2] == (code & 0xFF));
  f.hour = 4;
  CHECK(lt::rcdiy::dateHourCode(f) == code + 1);
}

TEST(rcdiy_can_and_imu_payloads) {
  uint8_t c[20];
  const uint8_t d[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  const size_t cn = lt::rcdiy::encodeCan(0x0618A001, d, 8, c);
  CHECK(cn == 12 && c[0] == 0x01 && c[1] == 0xA0 && c[2] == 0x18 && c[3] == 0x06 && std::memcmp(c + 4, d, 8) == 0);
  CHECK(lt::rcdiy::encodeCan(0x1FFFFF01, d, 3, c) == 7 && c[3] == 0x1F && c[4] == 0x11 && c[6] == 0x33);
  CHECK(lt::rcdiy::encodeCan(0x100, d, 0, c) == 4);
  // IMU: PID 0618A0F1, mg as int16 LE (forward, right, up)
  size_t n = lt::rcdiy::encodeImuAccel(0.3f, -0.25f, 0.012f, c);
  CHECK(n == 10 && c[0] == 0xF1 && c[1] == 0xA0 && c[2] == 0x18 && c[3] == 0x06);
  CHECK(i16le(c + 4) == 300 && i16le(c + 6) == -250 && i16le(c + 8) == 12);
  n = lt::rcdiy::encodeImuAccel(40.0f, -40.0f, 0.0f, c);
  CHECK(n == 10 && i16le(c + 4) == 32767 && i16le(c + 6) == -32768 && i16le(c + 8) == 0);
  // yaw rate: PID 0618A0F2, 0.01 deg/s, left turn positive, then two zeros
  n = lt::rcdiy::encodeImuGyro(12.34f, c);
  CHECK(n == 10 && c[0] == 0xF2 && i16le(c + 4) == 1234 && i16le(c + 6) == 0 && i16le(c + 8) == 0);
  n = lt::rcdiy::encodeImuGyro(-500.0f, c);
  CHECK(i16le(c + 4) == -32768);
  CHECK(lt::rcdiy::clampI16(1.5f) == 2 && lt::rcdiy::clampI16(-1.5f) == -2 && lt::rcdiy::clampI16(0.4f) == 0);
}

TEST(rcdiy_pid_filter) {
  lt::rcdiy::Filter flt;
  CHECK(!flt.pass(0x0618A001, 1000) && flt.count() == 0);  // deny all at first
  const uint8_t allow[] = {2, 0x00, 0x64, 0x06, 0x18, 0xA0, 0x01};  // 0618A001 every 100 ms
  flt.command(allow, sizeof(allow));
  CHECK(flt.count() == 1 && flt.pass(0x0618A001, 1000));
  CHECK(!flt.pass(0x0618A001, 1050));
  CHECK(flt.pass(0x0618A001, 1100));
  CHECK(!flt.pass(0x0210A006, 1100));
  CHECK(flt.allowed(0x0618A001) && !flt.allowed(0x0618A0F1));
  const uint8_t allow2[] = {2, 0x00, 0x00, 0x02, 0x10, 0xA0, 0x06};  // interval 0 = every time
  flt.command(allow2, sizeof(allow2));
  CHECK(flt.count() == 2 && flt.pass(0x0210A006, 1100) && flt.pass(0x0210A006, 1101));
  const uint8_t allow3[] = {2, 0x01, 0xF4, 0x06, 0x18, 0xA0, 0x01};  // the same PID, 500 ms
  flt.command(allow3, sizeof(allow3));
  CHECK(flt.count() == 2 && !flt.pass(0x0618A001, 1500) && flt.pass(0x0618A001, 1600));
  const uint8_t deny[] = {0};
  flt.command(deny, 1);
  CHECK(flt.count() == 0 && !flt.pass(0x0618A001, 2000) && !flt.pass(0x0210A006, 2000));
  const uint8_t all[] = {1, 0x00, 0xC8};  // allow all, 200 ms
  flt.command(all, 3);
  CHECK(flt.count() == -1 && flt.pass(0x12345678, 3000) && !flt.pass(0x12345678, 3100) &&
        flt.pass(0x12345679, 3100) && flt.pass(0x12345678, 3200));
  const uint8_t all0[] = {1, 0x00, 0x00};
  flt.command(all0, 3);
  CHECK(flt.pass(0x1, 1) && flt.pass(0x1, 1));
  const uint8_t shortCmd[] = {2, 0x00};
  flt.command(shortCmd, 2);  // too short: ignored
  flt.command(nullptr, 0);
  flt.command(deny, 1);
  for (uint32_t i = 0; i < 60; i++) {  // table overflow (48)
    const uint8_t a[] = {2, 0, 0, static_cast<uint8_t>(i >> 24), static_cast<uint8_t>(i >> 16),
                         static_cast<uint8_t>(i >> 8), static_cast<uint8_t>(i)};
    flt.command(a, 7);
  }
  CHECK(flt.count() == 48 && flt.pass(47, 5000) && !flt.pass(59, 5000));
  // car CAN: never faster than RC_CAN_MIN_MS
  flt.reset();
  const uint8_t allow20[] = {2, 0x00, 0x14, 0x06, 0x18, 0xA0, 0x01};  // RaceChrono asks for 20 ms
  flt.command(allow20, sizeof(allow20));
  CHECK(flt.pass(0x0618A001, 6000, RC_CAN_MIN_MS) && !flt.pass(0x0618A001, 6020, RC_CAN_MIN_MS) &&
        !flt.pass(0x0618A001, 6049, RC_CAN_MIN_MS) && flt.pass(0x0618A001, 6050, RC_CAN_MIN_MS));
  CHECK(flt.pass(0x0618A001, 6070));  // without a minimum (IMU) the asked interval stays
  const uint8_t allow200[] = {2, 0x00, 0xC8, 0x02, 0x10, 0xA0, 0x06};
  flt.command(allow200, sizeof(allow200));
  CHECK(flt.pass(0x0210A006, 7000, RC_CAN_MIN_MS) && !flt.pass(0x0210A006, 7100, RC_CAN_MIN_MS) &&
        flt.pass(0x0210A006, 7200, RC_CAN_MIN_MS));
  flt.command(all0, 3);
  CHECK(flt.pass(0x77, 8000, RC_CAN_MIN_MS) && !flt.pass(0x77, 8030, RC_CAN_MIN_MS) &&
        flt.pass(0x77, 8050, RC_CAN_MIN_MS) && flt.pass(0x78, 8050, RC_CAN_MIN_MS));
  CHECK(lt::rcdiy::PID_IMU_ACCEL == 0x0618A0F1 && lt::rcdiy::PID_IMU_GYRO == 0x0618A0F2 && RC_CAN_MIN_MS == 50);
}

TEST(rcdiy_gps_time_schedule) {
  lt::rcdiy::GpsTimeSync s;
  lt::GnssFix f = tokyoFix();
  uint32_t t = 100000;
  CHECK(s.update(f, t) && s.sync() == 1);  // first fix: new code, sync advances, time is sent
  CHECK(!s.update(f, t += 40) && s.sync() == 1);
  int sent = 0;
  for (int i = 0; i < 24; i++) sent += s.update(f, t += 40) ? 1 : 0;
  CHECK(sent == 1 && s.sync() == 1);  // once per second (at 1000 ms)
  f.hour = 4;  // next hour: new code
  CHECK(s.update(f, t += 40) && s.sync() == 2);
  for (int k = 0; k < 6; k++) {
    f.hour = static_cast<uint8_t>(5 + k);
    s.update(f, t += 40);
  }
  CHECK(s.sync() == 0);  // 3 bits wrap
  s.reset();
  CHECK(s.sync() == 0 && s.update(f, t) && s.sync() == 1);
}
