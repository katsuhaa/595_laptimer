// core/veh.h - values of the car from its raw CAN frames, as the round gauge does it
// (katsuhaa/abarth595-round-gauge 05ab0a1, meter_display/app.cpp `veh`, config.h VEH_*; the round
// gauge moved this from c6_can_link 1.1.0 in its 2.0.0). Same ids, scaling, timeouts and rules.
// Sources (all 29-bit ids; "confirmed" = checked with the owner's logs of the car):
//   rpm       C-CAN 0618A001 b2-b3 (big endian, rpm)                      confirmed 9/7
//   throttle  C-CAN 0618A001 b7 (0..255 = 0..100 %)                        confirmed 9/7
//   speed     C-CAN 0210A006 b4-b5 (big endian) / 128 = km/h               confirmed 9/7
//   clutch    C-CAN 0628A001 b5 bit5 (1 = pressed); only for the gear rule confirmed 9/7
//   water     C-CAN 0A18A001 b3 - 40 = degC                                confirmed 10/22
//   boost     C-CAN 0A1CA001 b2 * VEH_BOOST_BAR_PER_LSB = bar (gauge)      confirmed 10/25
//   reverse   C-CAN 0A18A001 b7 bit2 (1 = R)                               confirmed 10/22
//   fuel      C-CAN 0A18A000 b5 = % (0 and > 100 are not values)           confirmed 10/22
//   sport     C-CAN 0A18A000 b4 & 0x10                                     confirmed 10/28
//   light     B-CAN 06214000 b0 & 0x08 (light switch)                      confirmed 10/26
//   gear      R from the bit above; 1-5 from rpm / speed (VEH_GEAR_*); below 2 km/h N
// Not thread-safe: onFrame and getValues/lightInfo from different tasks need a lock around them.
#pragma once

#include <cstdint>

#include "meter_link.h"
#include "mlink.h"

namespace lt {

// config.h of the round gauge
constexpr uint32_t VEH_ID_RPM_ACCEL = 0x0618A001UL;
constexpr uint32_t VEH_ID_SPEED = 0x0210A006UL;
constexpr uint32_t VEH_ID_CLUTCH = 0x0628A001UL;
constexpr uint32_t VEH_ID_ENGINE = 0x0A18A001UL;  // water, reverse
constexpr uint32_t VEH_ID_FUEL = 0x0A18A000UL;    // fuel (b5), sport (b4)
constexpr uint32_t VEH_ID_BOOST = 0x0A1CA001UL;   // boost (b2)
constexpr float VEH_BOOST_BAR_PER_LSB = 0.02f;
constexpr uint8_t VEH_REVERSE_MASK = 0x04;  // 0A18A001 b7
constexpr uint32_t VEH_ID_LIGHT = 0x06214000UL;  // B-CAN
constexpr int VEH_LIGHT_BYTE = 0;
constexpr uint8_t VEH_LIGHT_MASK = 0x08;
constexpr uint8_t VEH_SPORT_MASK = 0x10;  // 0A18A000 b4; "seen" uses VEH_FUEL_TIMEOUT_MS like fuel
// No signal after this long. c6_can_link resends unchanged ids every second, so these are longer.
constexpr uint32_t VEH_TIMEOUT_MS = 2000;  // rpm, throttle, speed, clutch, boost, reverse
constexpr uint32_t VEH_WATER_TIMEOUT_MS = 3000;
constexpr uint32_t VEH_FUEL_TIMEOUT_MS = 5000;  // frames with b5 = 0 count as seen
// Light: the first value after power-up is used at once; later a different value must stay
// VEH_LIGHT_SETTLE_MS (the old value not seen again) before it switches. After VEH_LIGHT_TIMEOUT_MS
// without frames it is "not received", but the state stays.
constexpr uint32_t VEH_LIGHT_SETTLE_MS = 300;   // provisional in the round gauge
constexpr uint32_t VEH_LIGHT_TIMEOUT_MS = 5000;  // provisional in the round gauge
// Gear estimate (provisional in the round gauge): rpm / km/h within +-VEH_GEAR_TOL_PCT of a gear.
// Published 595 (C510 5-speed) ratios, 205/40R17 circumference; not measured on this car.
inline constexpr float VEH_GEAR_RATIOS[5] = {3.909f, 2.238f, 1.520f, 1.156f, 0.872f};
constexpr float VEH_FINAL_DRIVE = 3.353f;
constexpr float VEH_TIRE_CIRC_M = 1.878f;
constexpr float VEH_GEAR_TOL_PCT = 12.0f;
constexpr float VEH_GEAR_MIN_KMH = 5.0f;  // slower: keep the previous gear
constexpr float VEH_STOP_KMH = 2.0f;      // slower: N
constexpr int VEH_GEAR_MIN_RPM = 500;
constexpr bool VEH_GEAR_HOLD_ON_CLUTCH = false;  // 0 in the round gauge (clutch bit is mostly 1 in 1st)

// Values for the screens and logs. A value whose *Ok is false has no signal.
struct VehValues {
  bool linkUp;      // getLinkValues only: a good link frame within LINK_TIMEOUT_MS
  bool fromSignals;  // getLinkValues only: taken from value frames (not from raw CAN)
  bool rpmOk, throttleOk, speedOk, clutchOk, waterOk, boostOk, reverseOk, fuelOk, gearOk, sportOk;
  int rpm;            // [rpm]
  float throttlePct;  // [%]
  float speedKmh;     // [km/h]
  bool clutch;        // pressed
  float waterC;       // [degC]
  float boostBar;     // gauge pressure [bar], negative = vacuum
  bool reverse;       // R engaged
  float fuelPct;      // [%]
  char gear;          // '1'..'5', 'N', 'R' (0 = unknown, gearOk false)
  bool sport;         // sport mode
  bool sim;           // value frame marked ML_F_SIM (not the car)
  uint8_t scene;      // scene number of simulated data
};

// Fuel details (round gauge veh::FuelInfo)
struct FuelInfo {
  bool seen;         // 0A18A000 within VEH_FUEL_TIMEOUT_MS (b5 = 0 included)
  bool have;         // a usable value (1..100) was received
  uint8_t pct;       // value in use [%]
  uint8_t lastB5;    // last b5 (0 included)
  uint32_t ignored;  // frames whose b5 was dropped (0 or > 100)
};

// Light switch details (round gauge veh::LightInfo)
struct LightInfo {
  bool have;         // received since power-up (false = unknown)
  bool seen;         // received within VEH_LIGHT_TIMEOUT_MS
  bool on;           // light switch on
  uint8_t lastB0;    // last b0
  uint32_t firstMs;  // time of the first frame (when have)
  uint32_t changes;  // switches (the first value is not counted)
};

class VehDecoder {
 public:
  VehDecoder() { begin(); }
  void begin();  // forget everything (power-up state)
  // One raw CAN frame (bus 0 = C-CAN, 1 = B-CAN; ext 1 = 29-bit id)
  void onFrame(uint8_t bus, uint8_t ext, uint32_t id, uint8_t dlc, const uint8_t* d, uint32_t nowMs);
  // Values decoded from the raw frames (linkUp, fromSignals, sim, scene stay false/0). Updates the
  // gear estimate like the round gauge's veh::getSignals (call it for every displayed sample).
  void getValues(uint32_t nowMs, VehValues* out);
  // Like the round gauge's mlink::getSignals: value frames when one arrived within LINK_TIMEOUT_MS,
  // else the values from raw CAN when the link is up, else everything "no signal".
  void getLinkValues(const mlink::Parser& link, uint32_t nowMs, VehValues* out);
  void fuelInfo(FuelInfo* out, uint32_t nowMs) const;
  void lightInfo(LightInfo* out, uint32_t nowMs);  // may complete a pending switch
  float lastRatio() const { return lastRatio_; }  // last rpm per km/h used for the gear

 private:
  struct State {
    uint16_t rpm;
    uint8_t accel;      // 0..255
    uint16_t speedRaw;  // km/h * 128
    bool clutch;
    int water;          // [degC]
    uint8_t boostRaw;   // 0A1CA001 b2
    bool reverse;
    uint32_t rpmMs, speedMs, clutchMs, engMs, boostMs;  // last frame (0 = never)
    uint32_t fuelSeenMs;  // last 0A18A000 (b5 = 0 included)
    uint8_t fuelPct;
    uint8_t fuelLastB5;
    bool fuelHave;
    uint32_t fuelIgnored;
    bool sport;
    uint32_t lightSeenMs;
    uint32_t lightFirstMs;
    uint8_t lightLastB0;
    bool lightOn;
    bool lightPend;       // a different value arrived; waiting VEH_LIGHT_SETTLE_MS
    uint32_t lightPendMs;
    uint32_t lightChanges;
  };
  void lightSettle(uint32_t nowMs);
  void updateGear(bool rpmOk, bool speedOk, bool clutchOk);

  State g_{};
  char gear_ = 0;  // '1'..'5', 'N', 0 = unknown
  float lastRatio_ = 0.0f;
};

// Value frame (MlSignals) to values, the way the round gauge's mlink::getSignals reads it
// (the gear only when it is '1'..'9', 'N' or 'R'). linkUp and fromSignals are set true.
void vehFromSignals(const MlSignals& s, VehValues* out);

}  // namespace lt
