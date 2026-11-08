// Tests of core/veh (car values from raw CAN). Ported from the round gauge's pc_test/unit_main.cpp
// sections 22, 22a, 22b and 30 (katsuhaa/abarth595-round-gauge 05ab0a1). The real CAN logs of the car
// are read from the round gauge checkout when it exists (container path); otherwise those tests SKIP.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/meter_link.h"
#include "core/mlink.h"
#include "core/veh.h"
#include "tmini.h"

namespace {

using lt::mlink::LINK_TIMEOUT_MS;

const char* const kLogDir = "/home/claude/third_party/abarth595-round-gauge/logs";

std::vector<uint8_t> frame(uint8_t type, uint8_t seq, const void* p, uint8_t len) {
  uint8_t buf[ML_MAX_PAYLOAD + ML_OVERHEAD];
  const size_t n = mlBuild(buf, type, seq, p, len);
  return std::vector<uint8_t>(buf, buf + n);
}

MlSignals sigMsg(uint16_t rpm) {
  MlSignals s;
  std::memset(&s, 0, sizeof(s));
  s.valid = ML_V_RPM | ML_V_SPEED | ML_V_THROTTLE | ML_V_WATER | ML_V_FUEL | ML_V_GEAR | ML_V_BOOST;
  s.rpm = rpm;
  s.speed_c = 6000;
  s.throttle_h = 200;
  s.fuel_h = 124;
  s.water_d = 925;
  s.gear = '3';
  s.boost_c = -45;
  return s;
}

// The receive path of the firmware: link parser -> sink -> decoder
struct Rx : lt::mlink::Sink {
  lt::VehDecoder dec;
  lt::mlink::Parser link{this};
  uint8_t seq = 0;
  void onCan(uint8_t bus, uint8_t ext, uint32_t id, uint8_t dlc, const uint8_t* d, uint32_t t) override {
    dec.onFrame(bus, ext, id, dlc, d, t);
  }
  void can(uint8_t bus, uint32_t id, const uint8_t* d, uint8_t dlc, uint32_t t) {
    uint8_t p[sizeof(MlCanHead) + 8];
    MlCanHead h;
    h.info = static_cast<uint8_t>((bus ? 0x80 : 0) | 0x40 | (dlc & 0x0F));  // 29-bit id
    h.id = id;
    std::memcpy(p, &h, sizeof(h));
    std::memcpy(p + sizeof(h), d, dlc);
    const auto f = frame(ML_TYPE_CAN, seq++, p, static_cast<uint8_t>(sizeof(h) + dlc));
    link.feed(f.data(), f.size(), t);
  }
  lt::VehValues values(uint32_t t) {
    lt::VehValues v;
    dec.getLinkValues(link, t, &v);
    return v;
  }
};

bool fileExists(const char* path) {
  FILE* fp = std::fopen(path, "r");
  if (!fp) return false;
  std::fclose(fp);
  return true;
}

// One line of the c6_can_link log: "L <C|B> <millis> <id hex> <len> <bytes hex...>"
bool parseLine(const char* line, char* bus, unsigned long* tm, unsigned* id, unsigned* len, uint8_t d[8]) {
  unsigned b[8] = {0};
  if (std::sscanf(line, "L %c %lu %x %u %x %x %x %x %x %x %x %x", bus, tm, id, len, &b[0], &b[1], &b[2],
                  &b[3], &b[4], &b[5], &b[6], &b[7]) < 4 ||
      *len > 8) {
    return false;
  }
  for (unsigned k = 0; k < 8; k++) d[k] = k < *len ? static_cast<uint8_t>(b[k]) : 0;
  return true;
}

}  // namespace

TEST(veh_raw_values_like_round_gauge) {
  Rx rx;
  uint32_t t = 500000;
  const uint8_t rpm3000[8] = {0, 0, 0x0B, 0xB8, 0, 0, 0, 255};  // 3000 rpm, throttle 255 (100 %)
  const uint8_t kmh60[8] = {0, 0, 0, 0, 0x1E, 0x00, 0, 0};     // 7680 / 128 = 60 km/h
  const uint8_t eng[8] = {0, 0, 0, 130, 65, 0, 0, 0};          // water 130 - 40 = 90 degC
  const uint8_t trb[8] = {0, 1, 68, 0x48, 0x31, 0x10, 1, 0x80};  // boost 68 * 0.02 = 1.36 bar
  const uint8_t clutch[8] = {0, 0, 0, 0, 0, 0x20, 0, 0};
  rx.can(0, 0x0618A001, rpm3000, 8, t);
  rx.can(0, 0x0210A006, kmh60, 8, t);
  rx.can(0, 0x0A18A001, eng, 8, t);
  rx.can(0, 0x0628A001, clutch, 8, t);
  lt::VehValues s = rx.values(t);
  CHECK(s.linkUp && !s.fromSignals && rx.link.usingRaw(t) && s.rpmOk && s.rpm == 3000 && s.throttleOk &&
        s.throttlePct == 100.0f);
  CHECK(s.speedOk && s.speedKmh == 60.0f);
  CHECK(s.waterOk && s.waterC == 90.0f && !s.boostOk);  // 0A18A001 b4 is not boost
  CHECK(s.clutchOk && s.clutch && s.reverseOk && !s.reverse);
  rx.can(0, 0x0A1CA001, trb, 8, t);
  s = rx.values(t);
  CHECK(s.boostOk && std::fabs(s.boostBar - 1.36f) < 0.0001f);
  CHECK(s.gearOk && s.gear == '3');  // 3000 / 60 = 50 -> 3rd (the clutch does not hold it)
  CHECKF(std::fabs(rx.dec.lastRatio() - 50.0f) < 0.01f, "ratio %.2f", static_cast<double>(rx.dec.lastRatio()));
  CHECK(!s.fuelOk && !s.sim && s.scene == 0);
  const uint8_t engR[8] = {0, 0, 0, 130, 0, 0, 0, 0x04};
  rx.can(0, 0x0A18A001, engR, 8, t + 20);
  s = rx.values(t + 20);
  CHECK(s.gearOk && s.gear == 'R' && s.reverse);  // the R bit wins over the estimate
  const uint8_t kmh1[8] = {0, 0, 0, 0, 0x00, 0x80, 0, 0};  // 1 km/h
  rx.can(0, 0x0A18A001, eng, 8, t + 40);
  rx.can(0, 0x0210A006, kmh1, 8, t + 40);
  s = rx.values(t + 40);
  CHECK(s.gearOk && s.gear == 'N');  // below 2 km/h
  const uint8_t rpm5400[8] = {0, 0, 0x15, 0x18, 0, 0, 0, 0};
  rx.can(0, 0x0618A001, rpm5400, 8, t + 60);
  rx.can(0, 0x0210A006, kmh60, 8, t + 60);
  s = rx.values(t + 60);
  CHECK(!s.gearOk && s.throttlePct == 0.0f);  // 90 rpm per km/h: between 1st (116) and 2nd (67)

  // timeouts (other frames keep the link up)
  const uint8_t fuelZero[8] = {0x00, 0x09, 0x18, 0x7B, 0x02, 0x00, 0x00, 0x3D};
  rx.can(0, 0x0A18A000, fuelZero, 8, t + 60 + lt::VEH_TIMEOUT_MS);
  s = rx.values(t + 60 + lt::VEH_TIMEOUT_MS);
  CHECK(s.linkUp && s.rpmOk && s.speedOk);  // up to VEH_TIMEOUT_MS
  s = rx.values(t + 61 + lt::VEH_TIMEOUT_MS);
  CHECK(s.linkUp && !s.rpmOk && !s.speedOk && !s.throttleOk && !s.gearOk && !s.boostOk && s.waterOk);
  CHECK(!s.fuelOk);  // b5 = 0 alone (engine not started) is no value
  lt::FuelInfo fi;
  rx.dec.fuelInfo(&fi, t + 61 + lt::VEH_TIMEOUT_MS);
  CHECK(fi.ignored == 1 && !fi.have && fi.seen);
  uint8_t fuelV[8] = {0x20, 0x0D, 0x40, 0x7B, 0x00, 31, 0x20, 0x3D};
  rx.can(0, 0x0A18A000, fuelV, 8, t + 1060 + lt::VEH_TIMEOUT_MS);
  s = rx.values(t + 1060 + lt::VEH_TIMEOUT_MS);
  CHECK(s.fuelOk && s.fuelPct == 31.0f);  // b5 as %, no smoothing
  const uint32_t t2 = t + 100000;
  rx.can(0, 0x0A18A000, fuelZero, 8, t2);
  s = rx.values(t2);
  CHECK(s.fuelOk && s.fuelPct == 31.0f);  // engine off (b5 = 0): previous value
  fuelV[5] = 100;
  rx.can(0, 0x0A18A000, fuelV, 8, t2 + 1000);
  s = rx.values(t2 + 1000);
  CHECK(s.fuelOk && s.fuelPct == 100.0f);  // refuelled: at once
  fuelV[5] = 0xFF;
  rx.can(0, 0x0A18A000, fuelV, 8, t2 + 2000);
  rx.can(1, 0x0A18A000, fuelZero, 8, t2 + 2000);  // the same id on B-CAN is not used
  fuelV[5] = 50;
  rx.can(1, 0x0A18A000, fuelV, 8, t2 + 2000);
  s = rx.values(t2 + 2000);
  rx.dec.fuelInfo(&fi, t2 + 2000);
  CHECKF(s.fuelPct == 100.0f && fi.ignored == 3 && fi.lastB5 == 0xFF, "ignored %u last %02X", fi.ignored,
         fi.lastB5);
  // nothing else keeps the link up here, so ask the decoder directly
  rx.dec.getValues(t2 + 2001 + lt::VEH_FUEL_TIMEOUT_MS, &s);
  CHECK(!s.fuelOk);
  rx.dec.begin();
  rx.can(0, 0x0A18A000, fuelZero, 8, t2 + 3000);
  s = rx.values(t2 + 3000);
  CHECK(!s.fuelOk);  // after power-up no value until the engine runs (not saved)

  // value frames win while they arrive (c6_display_debug, demo, 1.x c6_can_link)
  const MlSignals m = sigMsg(4200);
  const auto f = frame(ML_TYPE_SIGNALS, rx.seq++, &m, sizeof(m));
  rx.link.feed(f.data(), f.size(), t2 + 5000);
  rx.can(0, 0x0618A001, rpm3000, 8, t2 + 5000);
  s = rx.values(t2 + 5000);
  CHECK(s.linkUp && s.fromSignals && !rx.link.usingRaw(t2 + 5000) && s.rpm == 4200 && s.gear == '3' &&
        s.fuelPct == 62.0f);
  rx.can(0, 0x0618A001, rpm3000, 8, t2 + 5000 + LINK_TIMEOUT_MS + 1);
  s = rx.values(t2 + 5000 + LINK_TIMEOUT_MS + 1);
  CHECK(s.linkUp && !s.fromSignals && rx.link.usingRaw(t2 + 5000 + LINK_TIMEOUT_MS + 1) && s.rpm == 3000);
  s = rx.values(t2 + 5002 + 2 * LINK_TIMEOUT_MS);
  CHECK(!s.linkUp && !s.rpmOk);
}

TEST(veh_value_frame_conversion) {
  MlSignals m = sigMsg(4200);
  lt::VehValues v;
  lt::vehFromSignals(m, &v);
  CHECK(v.linkUp && v.fromSignals && v.rpmOk && v.rpm == 4200);
  CHECK(v.waterOk && v.waterC > 92.4f && v.waterC < 92.6f && v.fuelPct == 62.0f && v.gear == '3');
  CHECK(v.speedKmh == 60.0f && v.throttlePct == 100.0f);
  CHECK(v.boostOk && v.boostBar > -0.451f && v.boostBar < -0.449f);  // vacuum
  m.gear = 'x';
  lt::vehFromSignals(m, &v);
  CHECK(!v.gearOk);  // only '1'..'9', 'N', 'R'
  m.gear = 'R';
  m.flags = ML_F_SIM;
  m.scene = 6;
  lt::vehFromSignals(m, &v);
  CHECK(v.gearOk && v.reverse && v.sim && v.scene == 6);
  m.valid = 0;
  lt::vehFromSignals(m, &v);
  CHECK(!v.rpmOk && !v.speedOk && !v.gearOk && !v.boostOk && !v.fuelOk);
}

TEST(veh_light_switch_settle_and_timeout) {
  Rx rx;
  lt::LightInfo li;
  uint32_t tl = 650000;
  rx.dec.lightInfo(&li, tl);
  CHECK(!li.have && !li.seen && !li.on && li.changes == 0);  // unknown (treated as off)
  uint8_t lt8[8] = {0x20, 0x00, 0x40, 0x00, 0x00, 0x55, 0x0B, 0x00};  // owner's photo 1 (off)
  rx.can(0, 0x06214000, lt8, 8, tl);  // the same id on C-CAN is not used
  rx.dec.lightInfo(&li, tl);
  CHECK(!li.have);
  rx.can(1, 0x06214000, lt8, 8, tl);
  rx.dec.lightInfo(&li, tl);
  CHECK(li.have && li.seen && !li.on && li.lastB0 == 0x20 && li.firstMs == tl && li.changes == 0);
  lt8[0] = 0x28;  // photo 2 (on)
  rx.can(1, 0x06214000, lt8, 8, tl += 1000);
  rx.dec.lightInfo(&li, tl + lt::VEH_LIGHT_SETTLE_MS - 1);
  CHECK(!li.on && li.lastB0 == 0x28);
  rx.dec.lightInfo(&li, tl + lt::VEH_LIGHT_SETTLE_MS);
  CHECK(li.on && li.changes == 1);  // switches without waiting for the next frame
  lt8[0] = 0x08;  // driving: 0x20 is 0, still on
  rx.dec.onFrame(1, 1, lt::VEH_ID_LIGHT, 8, lt8, tl += 1000);
  rx.dec.lightInfo(&li, tl + 1000);
  CHECK(li.on && li.changes == 1);
  lt8[0] = 0xF7;  // everything but 0x08 = off
  rx.dec.onFrame(1, 1, lt::VEH_ID_LIGHT, 8, lt8, tl += 1000);
  rx.dec.onFrame(1, 1, lt::VEH_ID_LIGHT, 8, lt8, tl + 100);
  lt8[0] = 0x08;
  rx.dec.onFrame(1, 1, lt::VEH_ID_LIGHT, 8, lt8, tl + 200);  // back within 0.2 s
  rx.dec.lightInfo(&li, tl + 1000);
  CHECK(li.on && li.changes == 1);
  lt8[0] = 0x20;
  rx.dec.onFrame(1, 1, lt::VEH_ID_LIGHT, 8, lt8, tl += 2000);
  rx.dec.onFrame(1, 1, lt::VEH_ID_LIGHT, 8, lt8, tl + 150);
  rx.dec.onFrame(1, 1, lt::VEH_ID_LIGHT, 8, lt8, tl + lt::VEH_LIGHT_SETTLE_MS + 50);
  rx.dec.lightInfo(&li, tl + lt::VEH_LIGHT_SETTLE_MS + 50);
  CHECK(!li.on && li.changes == 2);
  tl += lt::VEH_LIGHT_SETTLE_MS + 50;
  rx.dec.lightInfo(&li, tl + lt::VEH_LIGHT_TIMEOUT_MS);
  CHECK(li.have && li.seen && !li.on);
  lt8[0] = 0x28;
  rx.dec.onFrame(1, 1, lt::VEH_ID_LIGHT, 8, lt8, tl += 1000);
  rx.dec.lightInfo(&li, tl + lt::VEH_LIGHT_TIMEOUT_MS + 1);
  CHECK(li.have && !li.seen && li.on && li.lastB0 == 0x28 && li.changes == 3);  // last state kept
  rx.dec.lightInfo(&li, tl - 100);  // stamp newer than now: still seen
  CHECK(li.seen);
  rx.dec.onFrame(1, 0, lt::VEH_ID_LIGHT, 8, lt8, tl + 20000);
  rx.dec.onFrame(1, 1, lt::VEH_ID_LIGHT, 0, lt8, tl + 20000);
  rx.dec.onFrame(1, 1, lt::VEH_ID_LIGHT + 1, 8, lt8, tl + 20000);
  rx.dec.lightInfo(&li, tl + 20000);
  CHECK(!li.seen && li.changes == 3);  // 11-bit id, length 0, other ids: not used
  const uint8_t one[1] = {0x00};
  rx.dec.onFrame(1, 1, lt::VEH_ID_LIGHT, 1, one, tl + 21000);
  rx.dec.lightInfo(&li, tl + 21000 + lt::VEH_LIGHT_SETTLE_MS);
  CHECK(li.seen && !li.on && li.changes == 4);  // length 1 is enough for b0
  rx.dec.begin();
  rx.dec.lightInfo(&li, tl + 22000);
  CHECK(!li.have && !li.on && li.changes == 0);  // not saved over a power cycle
}

TEST(veh_sport_mode) {
  lt::VehDecoder dec;
  const uint32_t t0 = 300000000;
  uint8_t fd[8] = {0x20, 0x09, 0x40, 0x72, 0x10, 0x37, 0x20, 0xC8};  // owner's photo IMG_1801 (b4 = 10)
  dec.onFrame(0, 1, lt::VEH_ID_FUEL, 8, fd, t0);
  lt::VehValues v;
  dec.getValues(t0, &v);
  CHECK(v.sportOk && v.sport && v.fuelOk && v.fuelPct == 0x37);
  fd[4] = 0x00;  // IMG_1802 (normal)
  dec.onFrame(0, 1, lt::VEH_ID_FUEL, 8, fd, t0 + 100);
  dec.getValues(t0 + 100, &v);
  CHECK(v.sportOk && !v.sport);
  fd[4] = 0xEF;  // all but 0x10
  dec.onFrame(0, 1, lt::VEH_ID_FUEL, 8, fd, t0 + 200);
  dec.getValues(t0 + 200, &v);
  CHECK(!v.sport);
  fd[4] = 0x10;
  dec.onFrame(1, 1, lt::VEH_ID_FUEL, 8, fd, t0 + 300);  // B-CAN: not used
  dec.getValues(t0 + 300, &v);
  CHECK(!v.sport);
  dec.onFrame(0, 1, lt::VEH_ID_FUEL, 8, fd, t0 + 400);
  dec.getValues(t0 + 400 + lt::VEH_FUEL_TIMEOUT_MS + 1, &v);
  CHECK(!v.sportOk && !v.sport);
}

TEST(veh_random_frames_stay_in_range) {
  lt::VehDecoder dec;
  uint8_t rnd[8];
  uint32_t r = 12345;
  const uint32_t t2 = 600000;
  int bad = 0;
  for (int i = 0; i < 20000; i++) {
    for (int b = 0; b < 8; b++) rnd[b] = static_cast<uint8_t>((r = r * 1103515245u + 12345u) >> 16);
    static const uint32_t kIds[] = {0x0618A001, 0x0210A006, 0x0628A001, 0x0A18A001,
                                    0x0A18A000, 0x0A1CA001, 0x12345678};
    const uint32_t id = kIds[(r >> 8) % 7];
    dec.onFrame(static_cast<uint8_t>((r >> 4) & 1), static_cast<uint8_t>((r >> 5) & 1), id,
                static_cast<uint8_t>((r >> 12) % 9), rnd, t2 + 10000 + static_cast<uint32_t>(i) * 7);
    if (i % 50 == 0) {
      lt::VehValues v;
      dec.getValues(t2 + 10000 + static_cast<uint32_t>(i) * 7, &v);
      if (!(v.fuelPct >= 0.0f && v.fuelPct <= 100.0f && v.speedKmh >= 0.0f && v.speedKmh < 512.0f &&
            v.throttlePct <= 100.0f && v.waterC >= -40.0f && v.waterC <= 215.0f && v.boostBar >= 0.0f &&
            v.boostBar <= 255 * lt::VEH_BOOST_BAR_PER_LSB + 0.01f)) {
        bad++;
      }
    }
  }
  CHECKF(bad == 0, "%d samples out of range", bad);
}

// Round gauge 22a: the owner's logs of 10/22 (reverse, boost, refuel)
TEST(veh_real_logs_fuel_boost_reverse) {
  struct LogRun {
    int lines;
    uint32_t ignored;
    float fuelMin[2], fuelMax[2];  // [0] before split, [1] from split (only when fuelOk)
    float boostMax;
    bool sawR, sawGear;
  };
  auto run = [](const char* path, uint32_t split, LogRun* out) -> bool {
    FILE* fp = std::fopen(path, "r");
    if (!fp) return false;
    lt::VehDecoder dec;
    *out = LogRun{};
    out->fuelMin[0] = out->fuelMin[1] = 1000.0f;
    out->fuelMax[0] = out->fuelMax[1] = -1.0f;
    char line[256];
    while (std::fgets(line, sizeof(line), fp)) {
      char bus;
      unsigned long tm;
      unsigned id, len;
      uint8_t d[8];
      if (!parseLine(line, &bus, &tm, &id, &len, d)) continue;
      dec.onFrame(bus == 'B' ? 1 : 0, 1, id, static_cast<uint8_t>(len), d, static_cast<uint32_t>(tm));
      out->lines++;
      lt::VehValues v;
      dec.getValues(static_cast<uint32_t>(tm), &v);
      const int k = tm >= split ? 1 : 0;
      if (v.fuelOk && v.fuelPct < out->fuelMin[k]) out->fuelMin[k] = v.fuelPct;
      if (v.fuelOk && v.fuelPct > out->fuelMax[k]) out->fuelMax[k] = v.fuelPct;
      if (v.boostOk && v.boostBar > out->boostMax) out->boostMax = v.boostBar;
      if (v.gearOk && v.gear == 'R') out->sawR = true;
      if (v.gearOk && v.gear >= '1' && v.gear <= '5') out->sawGear = true;
    }
    std::fclose(fp);
    lt::FuelInfo fi;
    dec.fuelInfo(&fi, 0);
    out->ignored = fi.ignored;
    return true;
  };
  char path[256];
  std::snprintf(path, sizeof(path), "%s/can_reverse.txt", kLogDir);
  if (!fileExists(path)) {
    tmini::info("SKIP: %s not found (the round gauge logs exist only in the container)", path);
    return;
  }
  LogRun lr;
  CHECK(run(path, UINT32_MAX, &lr));
  tmini::info("log can_reverse: %d lines, b5 dropped %u, fuel %.0f..%.0f %%, R %s", lr.lines,
              static_cast<unsigned>(lr.ignored), static_cast<double>(lr.fuelMin[0]),
              static_cast<double>(lr.fuelMax[0]), lr.sawR ? "yes" : "no");
  CHECK(lr.ignored >= 1 && lr.fuelMin[0] == 34.0f && lr.fuelMax[0] == 35.0f);
  CHECK(lr.sawR);
  std::snprintf(path, sizeof(path), "%s/can_boost.txt", kLogDir);
  if (run(path, UINT32_MAX, &lr)) {
    tmini::info("log can_boost: %d lines, boost max %.2f bar, gear %s, fuel %.0f..%.0f %%", lr.lines,
                static_cast<double>(lr.boostMax), lr.sawGear ? "yes" : "no",
                static_cast<double>(lr.fuelMin[0]), static_cast<double>(lr.fuelMax[0]));
    CHECK(std::fabs(lr.boostMax - 1.36f) < 0.001f && lr.sawGear);
    CHECK(lr.fuelMin[0] == 33.0f && lr.fuelMax[0] == 34.0f);
  } else {
    CHECKF(false, "%s missing", path);
  }
  std::snprintf(path, sizeof(path), "%s/can_fuel.txt", kLogDir);
  if (run(path, 848686, &lr)) {
    tmini::info("log can_fuel: %d lines, b5 dropped %u, fuel before %.0f..%.0f %%, after %.0f..%.0f %%",
                lr.lines, static_cast<unsigned>(lr.ignored), static_cast<double>(lr.fuelMin[0]),
                static_cast<double>(lr.fuelMax[0]), static_cast<double>(lr.fuelMin[1]),
                static_cast<double>(lr.fuelMax[1]));
    CHECK(lr.ignored >= 1 && lr.fuelMin[0] == 31.0f && lr.fuelMax[0] == 33.0f);
    CHECK(lr.fuelMin[1] == 100.0f && lr.fuelMax[1] == 100.0f);
    CHECK(std::fabs(lr.boostMax - 0.52f) < 0.001f);
  } else {
    CHECKF(false, "%s missing", path);
  }
}

// Round gauge 22b: light switch in the same logs
TEST(veh_real_logs_light) {
  struct LightRun {
    int frames;
    uint32_t changes, firstMs;
    bool endOn;
    int probe[4];  // state at the probe times: 1 on, 0 off, 2 unknown, -1 no line yet
  };
  auto runLight = [](const char* path, const uint32_t* probes, int np, LightRun* out) -> bool {
    FILE* fp = std::fopen(path, "r");
    if (!fp) return false;
    lt::VehDecoder dec;
    *out = LightRun{};
    for (int i = 0; i < 4; i++) out->probe[i] = -1;
    lt::LightInfo li{};
    char line[256];
    while (std::fgets(line, sizeof(line), fp)) {
      char bus;
      unsigned long tm;
      unsigned id, len;
      uint8_t d[8];
      if (!parseLine(line, &bus, &tm, &id, &len, d)) continue;
      dec.onFrame(bus == 'B' ? 1 : 0, 1, id, static_cast<uint8_t>(len), d, static_cast<uint32_t>(tm));
      if (bus == 'B' && id == lt::VEH_ID_LIGHT) out->frames++;
      dec.lightInfo(&li, static_cast<uint32_t>(tm));
      for (int i = 0; i < np && i < 4; i++) {
        if (out->probe[i] < 0 && tm >= probes[i]) out->probe[i] = li.have ? (li.on ? 1 : 0) : 2;
      }
    }
    std::fclose(fp);
    out->changes = li.changes;
    out->firstMs = li.firstMs;
    out->endOn = li.on;
    return true;
  };
  char path[256];
  std::snprintf(path, sizeof(path), "%s/can_reverse.txt", kLogDir);
  if (!fileExists(path)) {
    tmini::info("SKIP: %s not found (the round gauge logs exist only in the container)", path);
    return;
  }
  LightRun lr;
  const uint32_t pRev[2] = {117500, 150000};
  CHECK(runLight(path, pRev, 2, &lr));
  tmini::info("light can_reverse: %d frames, first %.1f s, %u changes, end %s", lr.frames,
              static_cast<double>(lr.firstMs) / 1000.0, static_cast<unsigned>(lr.changes), lr.endOn ? "on" : "off");
  CHECK(lr.firstMs == 116981 && lr.probe[0] == 0 && lr.probe[1] == 1 && lr.changes == 1 && lr.endOn);
  const uint32_t pBoost[3] = {900000, 1100000, 1320000};
  std::snprintf(path, sizeof(path), "%s/can_boost.txt", kLogDir);
  if (runLight(path, pBoost, 3, &lr)) {
    tmini::info("light can_boost: %d frames, first %.1f s, %u changes, end %s", lr.frames,
                static_cast<double>(lr.firstMs) / 1000.0, static_cast<unsigned>(lr.changes),
                lr.endOn ? "on" : "off");
    CHECK(lr.probe[0] == 0 && lr.probe[1] == 1 && lr.probe[2] == 0 && lr.changes == 2 && !lr.endOn);
  } else {
    CHECKF(false, "%s missing", path);
  }
  const uint32_t pFuel[4] = {400000, 800000, 1000000, 1125000};
  std::snprintf(path, sizeof(path), "%s/can_fuel.txt", kLogDir);
  if (runLight(path, pFuel, 4, &lr)) {
    tmini::info("light can_fuel: %d frames, first %.1f s, %u changes, end %s", lr.frames,
                static_cast<double>(lr.firstMs) / 1000.0, static_cast<unsigned>(lr.changes),
                lr.endOn ? "on" : "off");
    CHECK(lr.firstMs == 32576 && lr.probe[0] == 1 && lr.probe[1] == 0 && lr.probe[2] == 1 && lr.probe[3] == 0 &&
          lr.changes == 5 && !lr.endOn);
  } else {
    CHECKF(false, "%s missing", path);
  }
}
