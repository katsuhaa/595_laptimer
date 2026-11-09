#include "veh.h"

#include <cstring>

namespace lt {

namespace {

inline uint32_t stamp(uint32_t t) { return t ? t : 1; }  // 0 means "never"

// t within timeout of now. A stamp from the receive task can be newer than now (negative difference).
inline bool within(uint32_t t, uint32_t now, uint32_t timeout) {
  return t && static_cast<int32_t>(now - t) <= static_cast<int32_t>(timeout);
}

// Calculated rpm per km/h of a gear: 1 km/h = 1000 m/h -> 1000 / circumference turns per hour
// -> / 60 per minute, times the gear and final ratios
float rpmPerKmh(int gear) {
  return VEH_GEAR_RATIOS[gear] * VEH_FINAL_DRIVE * (1000.0f / VEH_TIRE_CIRC_M) / 60.0f;
}

}  // namespace

void VehDecoder::begin() {
  std::memset(&g_, 0, sizeof(g_));
  gear_ = 0;
  lastRatio_ = 0.0f;
}

// A value different from lightOn switches after VEH_LIGHT_SETTLE_MS unless the old value came back.
// Also called from lightInfo, so the switch does not wait for the next frame. A stamp newer than now
// (negative difference) waits.
void VehDecoder::lightSettle(uint32_t now) {
  if (g_.lightPend &&
      static_cast<int32_t>(now - g_.lightPendMs) >= static_cast<int32_t>(VEH_LIGHT_SETTLE_MS)) {
    g_.lightOn = !g_.lightOn;
    g_.lightPend = false;
    g_.lightChanges++;
  }
}

void VehDecoder::onFrame(uint8_t bus, uint8_t ext, uint32_t id, uint8_t dlc, const uint8_t* d,
                         uint32_t now) {
  if (!ext) return;  // every id used is 29-bit
  if (bus == 1) {
    if (id == VEH_ID_LIGHT && dlc > VEH_LIGHT_BYTE) {  // only the 0x08 bit of b0
      const bool on = (d[VEH_LIGHT_BYTE] & VEH_LIGHT_MASK) != 0;
      if (!g_.lightSeenMs) {  // first after power-up: use at once
        g_.lightOn = on;
        g_.lightFirstMs = stamp(now);
      } else {
        lightSettle(now);
        if (on == g_.lightOn) {
          g_.lightPend = false;  // the old value again: no switch
        } else if (!g_.lightPend) {
          g_.lightPend = true;
          g_.lightPendMs = stamp(now);
        }
      }
      g_.lightLastB0 = d[VEH_LIGHT_BYTE];
      g_.lightSeenMs = stamp(now);
    } else if (id == VEH_ID_ODO && dlc >= 4) {  // b1 low 4 bits + b2 + b3 = km
      const uint32_t km = (static_cast<uint32_t>(d[1] & 0x0F) << 16) |
                          (static_cast<uint32_t>(d[2]) << 8) | d[3];
      if (!g_.odoHave || (km >= g_.odoKm && km - g_.odoKm <= ODO_MAX_JUMP_KM)) {
        g_.odoKm = km;
        g_.odoHave = true;
        g_.odoMs = stamp(now);
        g_.odoCandN = 0;
      } else {
        g_.odoRejected++;
        if (g_.odoCandN && km == g_.odoCand) {
          g_.odoCandN++;
        } else {
          g_.odoCand = km;
          g_.odoCandN = 1;
        }
        if (g_.odoCandN >= ODO_ACCEPT_REPEAT) {
          g_.odoKm = km;
          g_.odoMs = stamp(now);
          g_.odoCandN = 0;
        }
      }
    }
    return;
  }
  if (bus != 0) return;
  if (id == VEH_ID_RPM_ACCEL && dlc >= 8) {
    g_.rpm = static_cast<uint16_t>((static_cast<uint16_t>(d[2]) << 8) | d[3]);
    g_.accel = d[7];
    g_.rpmMs = stamp(now);
  } else if (id == VEH_ID_SPEED && dlc >= 6) {
    g_.speedRaw = static_cast<uint16_t>((static_cast<uint16_t>(d[4]) << 8) | d[5]);
    g_.speedMs = stamp(now);
  } else if (id == VEH_ID_CLUTCH && dlc >= 6) {
    g_.clutch = (d[5] & 0x20) != 0;
    g_.clutchMs = stamp(now);
  } else if (id == VEH_ID_ENGINE && dlc >= 8) {
    g_.water = static_cast<int>(d[3]) - 40;
    g_.reverse = (d[7] & VEH_REVERSE_MASK) != 0;
    g_.engMs = stamp(now);
  } else if (id == VEH_ID_BOOST && dlc >= 3) {
    g_.boostRaw = d[2];
    g_.boostMs = stamp(now);
  } else if (id == VEH_ID_FUEL && dlc >= 6) {
    g_.fuelSeenMs = stamp(now);
    g_.fuelLastB5 = d[5];
    g_.sport = (d[4] & VEH_SPORT_MASK) != 0;
    if (d[5] == 0 || d[5] > 100) {
      g_.fuelIgnored++;  // 0 = not measured (engine off etc.): keep the previous value
    } else {
      g_.fuelPct = d[5];
      g_.fuelHave = true;
    }
  }
}

// Same as c6_can_link 1.1.0 updateGear / round gauge veh::updateGear
void VehDecoder::updateGear(bool rpmOk, bool speedOk, bool clutchOk) {
  if (!rpmOk || !speedOk) {
    gear_ = 0;
    return;
  }
  const float kmh = static_cast<float>(g_.speedRaw) / 128.0f;
  if (kmh < VEH_STOP_KMH) {
    gear_ = 'N';
    return;
  }
  if (kmh < VEH_GEAR_MIN_KMH || g_.rpm < VEH_GEAR_MIN_RPM) return;  // slow: keep the previous gear
  if (VEH_GEAR_HOLD_ON_CLUTCH && clutchOk && g_.clutch) return;
  const float ratio = static_cast<float>(g_.rpm) / kmh;
  lastRatio_ = ratio;
  for (int k = 0; k < 5; k++) {
    const float ref = rpmPerKmh(k);
    if (ratio >= ref * (1.0f - VEH_GEAR_TOL_PCT / 100.0f) &&
        ratio <= ref * (1.0f + VEH_GEAR_TOL_PCT / 100.0f)) {
      gear_ = static_cast<char>('1' + k);
      return;
    }
  }
  gear_ = 0;  // no gear fits (shifting)
}

void VehDecoder::getValues(uint32_t now, VehValues* out) {
  *out = VehValues{};
  const State& r = g_;
  const bool rpmOk = within(r.rpmMs, now, VEH_TIMEOUT_MS);
  const bool speedOk = within(r.speedMs, now, VEH_TIMEOUT_MS);
  const bool clutchOk = within(r.clutchMs, now, VEH_TIMEOUT_MS);
  const bool waterOk = within(r.engMs, now, VEH_WATER_TIMEOUT_MS);
  const bool engOk = within(r.engMs, now, VEH_TIMEOUT_MS);      // 0A18A001 (reverse)
  const bool boostOk = within(r.boostMs, now, VEH_TIMEOUT_MS);  // 0A1CA001
  const bool fuelOk = r.fuelHave && within(r.fuelSeenMs, now, VEH_FUEL_TIMEOUT_MS);
  updateGear(rpmOk, speedOk, clutchOk);
  const char gear = (engOk && r.reverse) ? 'R' : gear_;  // the R bit wins over the estimate

  // Same steps as the 1.x value frames, so screens and rules do not change
  out->rpmOk = rpmOk;
  out->throttleOk = rpmOk;
  out->speedOk = speedOk;
  out->clutchOk = clutchOk;
  out->waterOk = waterOk;
  out->boostOk = boostOk;
  out->reverseOk = engOk;
  out->fuelOk = fuelOk;
  out->gearOk = gear != 0;
  out->rpm = r.rpm;
  out->throttlePct =
      static_cast<float>(static_cast<uint8_t>((static_cast<uint32_t>(r.accel) * 200 + 127) / 255)) / 2.0f;
  out->speedKmh =
      static_cast<float>(static_cast<uint16_t>(static_cast<uint32_t>(r.speedRaw) * 100 / 128)) / 100.0f;
  out->clutch = r.clutch;
  const int w = r.water < -3276 ? -3276 : (r.water > 3276 ? 3276 : r.water);
  out->waterC = static_cast<float>(static_cast<int16_t>(w * 10)) / 10.0f;
  const int bc = static_cast<int>(static_cast<float>(r.boostRaw) * VEH_BOOST_BAR_PER_LSB * 100.0f + 0.5f);
  out->boostBar = static_cast<float>(static_cast<int16_t>(bc > 32767 ? 32767 : bc)) / 100.0f;
  out->reverse = r.reverse;
  out->fuelPct = r.fuelPct;
  out->gear = gear;
  out->sportOk = within(r.fuelSeenMs, now, VEH_FUEL_TIMEOUT_MS);  // 0A18A000 received
  out->sport = out->sportOk && r.sport;
}

void VehDecoder::getLinkValues(const mlink::Parser& link, uint32_t nowMs, VehValues* out) {
  if (link.signalsUp(nowMs)) {
    vehFromSignals(link.lastSignals(), out);
    return;
  }
  // No value frame (c6_can_link 2.x does not send them): decode the raw CAN frames
  const bool up = link.linkUp(nowMs);
  if (up) {
    getValues(nowMs, out);
  } else {
    *out = VehValues{};
  }
  out->linkUp = up;
}

void VehDecoder::fuelInfo(FuelInfo* out, uint32_t now) const {
  out->seen = within(g_.fuelSeenMs, now, VEH_FUEL_TIMEOUT_MS);
  out->have = g_.fuelHave;
  out->pct = g_.fuelPct;
  out->lastB5 = g_.fuelLastB5;
  out->ignored = g_.fuelIgnored;
}

void VehDecoder::lightInfo(LightInfo* out, uint32_t now) {
  lightSettle(now);
  out->have = g_.lightSeenMs != 0;
  out->seen = within(g_.lightSeenMs, now, VEH_LIGHT_TIMEOUT_MS);
  out->on = g_.lightOn;
  out->lastB0 = g_.lightLastB0;
  out->firstMs = g_.lightFirstMs;
  out->changes = g_.lightChanges;
}

void VehDecoder::odoInfo(OdoInfo* out, uint32_t now) const {
  out->have = g_.odoHave;
  out->seen = g_.odoHave && within(g_.odoMs, now, ODO_STALE_MS);
  out->km = g_.odoKm;
  out->rejected = g_.odoRejected;
}

void vehFromSignals(const MlSignals& s, VehValues* out) {
  *out = VehValues{};
  out->linkUp = true;
  out->fromSignals = true;
  out->rpmOk = (s.valid & ML_V_RPM) != 0;
  out->speedOk = (s.valid & ML_V_SPEED) != 0;
  out->throttleOk = (s.valid & ML_V_THROTTLE) != 0;
  out->waterOk = (s.valid & ML_V_WATER) != 0;
  out->fuelOk = (s.valid & ML_V_FUEL) != 0;
  out->boostOk = (s.valid & ML_V_BOOST) != 0;
  const char gear = s.gear;
  out->gearOk = (s.valid & ML_V_GEAR) && ((gear >= '1' && gear <= '9') || gear == 'N' || gear == 'R');
  out->rpm = s.rpm;
  out->speedKmh = static_cast<float>(s.speed_c) / 100.0f;
  out->throttlePct = static_cast<float>(s.throttle_h) / 2.0f;
  out->waterC = static_cast<float>(s.water_d) / 10.0f;
  out->fuelPct = static_cast<float>(s.fuel_h) / 2.0f;
  out->boostBar = static_cast<float>(s.boost_c) / 100.0f;
  out->gear = gear;
  out->reverse = out->gearOk && gear == 'R';
  out->reverseOk = out->gearOk;
  out->sim = (s.flags & ML_F_SIM) != 0;
  out->scene = s.scene;
}

}  // namespace lt
