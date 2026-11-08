// core/ubx.h - UBX of the GNSS receiver (Matek M9N-5883 = u-blox NEO-M9N on UART1).
// Parser: port of the round gauge's gnss (katsuhaa/abarth595-round-gauge 05ab0a1, meter_display/app.cpp
// `gnss`): the same byte state machine (sync B5 62, class, id, length, payload, Fletcher checksum;
// lengths over 1024 resync), NAV-PVT (92 bytes) decoding and ACK/NAK/MON-VER counting. Added for the
// square gadget: hAcc, vAcc, sAcc, headAcc, UTC in us, a FIFO of fixes (one log line per epoch),
// missing epochs from iTOW gaps, NMEA sentence counting (baud detection) and 1-second rates.
// Builders: CFG-VALSET in the RAM layer (the M9N loses it at power-off; the firmware sends it at every
// start) with the round gauge's configuration keys. Constellations stay at the receiver default; the
// round gauge's M10-only OTP procedure is not ported.
#pragma once

#include <cstddef>
#include <cstdint>

#include "lap.h"

namespace lt {

// One NAV-PVT. Fields of the round gauge's gnss::Fix, then the additions.
struct GnssFix {
  bool fixOk;        // position usable: gnssFixOK and fixType 2, 3 or 4
  uint8_t fixType;   // 0 none, 2 2D, 3 3D, 4 GNSS + dead reckoning, 5 time only
  uint8_t numSV;
  bool diffSoln;     // differential corrections (SBAS etc.)
  int32_t lat, lon;  // [1e-7 deg]
  int32_t hMSL;      // height above mean sea level [mm]
  int32_t gSpeed;    // ground speed [mm/s]
  int32_t headMot;   // heading of motion [1e-5 deg]
  uint16_t pDOP;     // [0.01]
  bool timeOk;       // validDate && validTime
  uint16_t year;
  uint8_t month, day, hour, min, sec;
  int32_t nano;      // fraction of the second [ns] (can be negative)
  uint32_t iTOW;     // GPS time of week [ms]
  uint32_t rxMs;     // local time the frame was completed (millis)
  // square gadget additions
  uint32_t hAcc_mm;    // horizontal accuracy estimate [mm]
  uint32_t vAcc_mm;    // vertical accuracy estimate [mm]
  uint32_t sAcc_mmps;  // speed accuracy estimate [mm/s]
  uint32_t headAcc;    // heading accuracy estimate [1e-5 deg]
  int64_t utcUs;       // UTC of the epoch [us since 1970-01-01] (0 unless timeOk)
  bool timeResolved;   // valid bit 2 fullyResolved (UTC fully known, leap seconds included)
};

// What the lap timer accepts as a usable epoch (lt::Fix::valid)
struct LapFixRules {
  uint8_t minFixType = 3;  // 3 = 3D (or 3D + DR); 2 also accepts 2D
  uint32_t maxHAccMm = 0;  // 0 = no limit (not decided yet)
};

// GnssFix -> lt::Fix for core/lap: tUs = GNSS UTC (utcUs), valid = fixOk, fixType >= minFixType,
// timeOk and hAcc within maxHAccMm
Fix toLapFix(const GnssFix& g, const LapFixRules& rules = LapFixRules());

namespace ubx {

// UBX ids
constexpr uint8_t CLS_NAV = 0x01, ID_NAV_PVT = 0x07;
constexpr uint8_t CLS_ACK = 0x05, ID_ACK_ACK = 0x01, ID_ACK_NAK = 0x00;
constexpr uint8_t CLS_CFG = 0x06, ID_CFG_VALSET = 0x8A;
constexpr uint8_t CLS_MON = 0x0A, ID_MON_VER = 0x04;
constexpr uint16_t NAV_PVT_LEN = 92;
constexpr uint16_t MAX_SKIP = 1024;  // longer frames are impossible here: resync

// Configuration keys (round gauge app.cpp, checked there against SparkFun u-blox_config_keys.h;
// the same ids are in the u-blox M9 interface description)
constexpr uint32_t KEY_RATE_MEAS = 0x30210001;         // U2 measurement period [ms]
constexpr uint32_t KEY_RATE_NAV = 0x30210002;          // U2 measurements per navigation solution
constexpr uint32_t KEY_MSGOUT_PVT_UART1 = 0x20910007;  // U1 NAV-PVT rate on UART1 (per epoch)
constexpr uint32_t KEY_UART1OUT_UBX = 0x10740001;      // L UBX output on UART1
constexpr uint32_t KEY_UART1OUT_NMEA = 0x10740002;     // L NMEA output on UART1
constexpr uint32_t KEY_UART1IN_UBX = 0x10730001;       // L UBX input on UART1
constexpr uint32_t KEY_DYNMODEL = 0x20110021;          // E1 dynamic platform model
constexpr uint32_t KEY_UART1_BAUD = 0x40520001;        // U4 UART1 baud rate
constexpr uint8_t LAYER_RAM = 0x01;                    // CFG-VALSET layers: bit0 RAM
constexpr uint8_t DYNMODEL_AUTOMOTIVE = 4;

constexpr int kFixQueue = 8;       // fixes kept for takeFix() (the newest win)
constexpr int kPayloadCap = 256;   // stored payload bytes (NAV-PVT 92, MON-VER 40 + 30 per extension)
constexpr int kMonVerExtMax = 6;   // MON-VER extension strings kept

struct Counters {
  uint32_t bytes;         // bytes fed
  uint32_t frames;        // UBX frames with a good checksum (any class)
  uint32_t pvt;           // NAV-PVT decoded
  uint32_t ack, nak;      // ACK-ACK / ACK-NAK (any message)
  uint32_t ackValset, nakValset;  // ACK-ACK / ACK-NAK for CFG-VALSET
  uint32_t monVer;        // MON-VER received
  uint32_t bad;           // UBX checksum errors
  uint32_t nmea;          // NMEA sentences with a good checksum
  uint32_t fixDropped;    // fixes overwritten before takeFix()
  uint32_t missedEpochs;  // epochs missing in iTOW gaps (only while an epoch period is set)
  uint32_t gaps;          // gaps (each gap of one or more missing epochs)
  uint32_t maxGapMs;      // longest step between epochs since the period was set
  uint32_t dupEpochs;     // NAV-PVT with the same iTOW as the previous one
  uint32_t timeJumps;     // iTOW steps that disagree with the local clock by > 1 s
  uint32_t pvtPerSec;     // NAV-PVT in the last full second (tick)
  uint32_t bytesPerSec;   // bytes in the last full second (tick)
};

class Parser {
 public:
  // epochPeriodMs: navigation period for the missing-epoch count (40 for 25 Hz; 0 = do not count)
  explicit Parser(uint32_t epochPeriodMs = 0) : periodMs_(epochPeriodMs) { begin(); }
  void begin();  // forget everything (keeps the epoch period)
  // Returns the number of NAV-PVT completed in these bytes
  int feed(const uint8_t* data, size_t n, uint32_t nowMs);
  bool takeFix(GnssFix* out);         // oldest fix not taken yet (FIFO of kFixQueue)
  bool latest(GnssFix* out) const;    // newest NAV-PVT (false before the first)
  // Period of the configured rate; restarts the gap reference (the next NAV-PVT is not compared)
  void setEpochPeriodMs(uint32_t ms);
  uint32_t epochPeriodMs() const { return periodMs_; }
  void tick(uint32_t nowMs);  // 1-second rates; call often
  const Counters& counters() const { return c_; }
  // MON-VER: swVersion (30 chars) and hwVersion (10 chars); false before the first MON-VER
  bool monVer(char* sw, size_t swCap, char* hw, size_t hwCap) const;
  int monVerExtCount() const { return monExtN_; }
  bool monVerExt(int i, char* out, size_t cap) const;  // e.g. "FWVER=SPG 4.04", "MOD=NEO-M9N"
  // Text after a prefix in the extensions ("MOD=" -> "NEO-M9N")
  bool monVerFind(const char* prefix, char* out, size_t cap) const;

 private:
  void dispatch(uint32_t nowMs);
  void decodePvt(const uint8_t* d, uint32_t nowMs);
  void countEpoch(const GnssFix& f);
  void nmeaByte(uint8_t b);

  uint32_t periodMs_ = 0;
  Counters c_{};
  // UBX state machine (round gauge gnss::feed): 0 sync1, 1 sync2, 2 class, 3 id, 4 len L, 5 len H,
  // 6 payload, 7 ck A, 8 ck B, 9 ck B after a bad ck A
  int state_ = 0;
  uint8_t cls_ = 0, id_ = 0, ckA_ = 0, ckB_ = 0;
  uint16_t plen_ = 0, pos_ = 0;
  uint8_t pay_[kPayloadCap]{};
  // fixes
  GnssFix last_{};
  bool have_ = false;
  GnssFix q_[kFixQueue]{};
  int qHead_ = 0, qN_ = 0;
  // epoch gaps
  bool epochRef_ = false;
  uint32_t prevItow_ = 0, prevRxMs_ = 0;
  // MON-VER
  char monSw_[31]{}, monHw_[11]{};
  char monExt_[kMonVerExtMax][31]{};
  int monExtN_ = 0;
  // NMEA: 0 idle, 1 body, 2 hex 1, 3 hex 2, 4 CR, 5 LF
  int nmeaState_ = 0;
  uint8_t nmeaXor_ = 0, nmeaCk_ = 0;
  int nmeaLen_ = 0, nmeaAddr_ = 0;
  bool nmeaInAddr_ = false;
  // tick
  uint32_t tickMs_ = 0, tickPvt_ = 0, tickBytes_ = 0;
  bool tickStarted_ = false;
};

// UBX frame: B5 62 cls id len(LE) payload ckA ckB. Returns the size, 0 when cap is too small.
size_t frame(uint8_t* out, size_t cap, uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len);
// CFG-VALSET (RAM layer): UART1 baud rate. 20 bytes.
size_t buildBaud(uint8_t* out, size_t cap, uint32_t baud);
// CFG-VALSET (RAM layer): measurement period 1000 / rateHz ms (40 ms minimum = 25 Hz), one solution
// per measurement, NAV-PVT on UART1 every epoch, NMEA output off and UBX in/out on for UART1,
// automotive dynamic model. Constellations are not touched. 49 bytes.
size_t buildNavConfig(uint8_t* out, size_t cap, int rateHz);
// MON-VER poll (no payload). 8 bytes.
size_t buildPollMonVer(uint8_t* out, size_t cap);

}  // namespace ubx
}  // namespace lt
