// settings.h - device settings kept in NVS (lists such as courses/maint/wifi live in /config/*.json on the XTSD).
#pragma once
#include <stdint.h>

namespace settings {

struct Settings {
  uint16_t version;
  // Shift indicator thresholds [rpm] (UI設計 §4.1: green 4500 / yellow 5500 / red 6000 / flash 6300)
  int16_t shiftGreen, shiftYellow, shiftRed, shiftFlash;
  uint8_t brightnessPct;   // normal brightness
  uint8_t illumPct;        // brightness while the light switch (B-CAN) is on (round gauge 2.9.0)
  int16_t tzMin;           // local time offset [min] (JST +540)
  uint8_t oilWarmBit;      // oil-temperature switch polarity: value of the bit that means "warm" (unconfirmed)
  uint8_t bleOn;           // RaceChrono DIY over BLE
  uint8_t canRawLog;       // write <id>_<cid>_can.csv (raw CAN, large)
  int64_t odoMm;           // odometer [mm] = value set by the owner + integrated speed (DriveState)
  int64_t trackMs;         // time on a registered course above 20 km/h [ms]
  uint8_t imuCalOk;
  float imuCal[9];         // lt::ImuCal export
};

void defaults(Settings* s);
bool load(Settings* s);         // false = defaults were used
bool save(const Settings& s);   // writes NVS (call rarely: setting changed, session closed, every 5 min while moving)

}  // namespace settings
