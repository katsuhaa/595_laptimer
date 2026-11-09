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
  // Round gauge settings (ui_spec.md 8, config.h SET_*; UI設計 v1.0 §6). The shift* thresholds above stay until the
  // lap and street screens use shiftRpm (実装計画 R5)
  int16_t shiftRpm;        // シフトポイント 3000..7500 / 100 (5800)
  uint8_t ringOne;         // 回転数の輪: 0 = 両側, 1 = 片側
  uint8_t autoReturnMin;   // 自動で戻る 1..10 min (3)
  int16_t warnTempC;       // 水温の警告 90..125 / 5 (110)
  uint8_t fuelWarnPct;     // 燃料の警告 5..50 / 5 (20)
  int16_t loadRpm;         // 過負荷 回転数 2000..6000 / 100 (3000): also the ▲▼ boundary
  uint8_t loadYellowMin;   // 過負荷時間 黄 0..90 / 5 (10, 0 = off)
  uint8_t loadRedMin;      // 過負荷時間 赤 0..90 / 5 (30, 0 = off)
  uint8_t loadResetMin;    // 過負荷がリセットされる時間 1..30 (5)
  uint8_t maintHours;      // メンテナンスまでの累積時間 0..99 h (5, 0 = off)
  uint8_t demoOn;          // デモ (0)
};

// Values kept every minute (round gauge saveTimes, TIME_SAVE_MS): ▲▼ and the overload time with the UTC of the save
struct Times {
  uint16_t version;
  uint32_t aboveMs, belowMs;  // ▲▼ 累積時間
  uint32_t loadMs;            // 過負荷時間
  uint32_t loadUtc;           // UTC of the save (0 = time unknown)
  uint32_t loadLowMs;         // time below 過負荷 回転数 at the save
};
bool loadTimes(Times* t);        // false: none saved (zeros)
bool saveTimes(const Times& t);

void defaults(Settings* s);
bool load(Settings* s);         // false = defaults were used
bool save(const Settings& s);   // writes NVS (call rarely: setting changed, session closed, every 5 min while moving)

}  // namespace settings
