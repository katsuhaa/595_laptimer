// core/csvlog.h - text of the session files (ARCH §5.1-§5.3): the main log CSV (header + one row per
// GNSS epoch), the laps CSV, the 200 Hz IMU CSV, the raw CAN CSV, meta.json, the session id and the
// file names. Every function writes into the caller's buffer and returns the length (without the
// NUL), or 0 when the buffer is too small. Lines end with LF.
#pragma once

#include <cstdint>

#include "lap.h"

namespace lt {
namespace csvlog {

// ---- main log: <id>_<cid>.csv --------------------------------------------------------------------
extern const char kMainHeader[];  // the exact header row of ARCH §5.2, with '\n'

// One row = one GNSS epoch. A value whose *Ok is false is written as an empty field.
struct MainRow {
  bool utcOk = false;
  int64_t utcMs = 0;  // GNSS time of the epoch [ms since 1970, UTC]
  bool posOk = false;
  int32_t latE7 = 0;  // written with 7 decimals (exact)
  int32_t lonE7 = 0;
  bool altOk = false;
  float altM = 0.0f;  // 1 decimal
  bool speedOk = false;
  float speedKmh = 0.0f;  // GNSS speed, 2 decimals
  bool headingOk = false;
  float headingDeg = 0.0f;  // 1 decimal
  bool haccOk = false;
  float haccM = 0.0f;  // 2 decimals
  bool satsOk = false;
  int sats = 0;
  bool fixOk = false;
  int fix = 0;  // 0 / 2 / 3
  bool imuOk = false;  // lon_g, lat_g, vert_g: car frame (forward+, right+, up+, gravity removed), 3 decimals
  float lonG = 0.0f;
  float latG = 0.0f;
  float vertG = 0.0f;
  bool yawOk = false;
  float yawDps = 0.0f;  // left turn +, 1 decimal
  bool rpmOk = false;
  int rpm = 0;
  bool throttleOk = false;
  float throttlePct = 0.0f;  // 1 decimal
  bool speedCanOk = false;
  float speedCanKmh = 0.0f;  // 2 decimals
  bool waterOk = false;
  float waterC = 0.0f;  // 1 decimal
  bool fuelOk = false;
  float fuelPct = 0.0f;  // 1 decimal
  bool boostOk = false;
  float boostBar = 0.0f;  // 2 decimals
  bool gearOk = false;
  char gear = 'N';  // '1'..'5', 'N', 'R' (anything else: empty)
  int lap = 0;      // 0 before the first S/F crossing
  int sector = 0;   // 1-based in the current lap, 0 before the first crossing
  bool lapMsOk = false;
  int32_t lapMs = 0;  // elapsed in the current lap (not before the first crossing)
  bool ghostOk = false;
  float ghostDeltaS = 0.0f;  // 3 decimals
};

int mainHeader(char* buf, int cap);
int mainLine(char* buf, int cap, const MainRow& r);

// The lap timer's input for this row, built from the values exactly as mainLine() writes them
// (tUs = utc_ms * 1000, speed rounded to 0.01 km/h, valid = time + position + speed + fix 3).
// Feed the live LapTimer/Ghost with this, and replay (lt::Replay) gives bit-identical fixes and laps.
Fix fixOf(const MainRow& r);
// The rule behind fixOf() with the speed given as the written digits (0.01 km/h). lt::Replay uses
// it with the digits read from the file: one rule for live and replay (change it only here).
Fix fixFromCentiKmh(const MainRow& r, bool speedOk, int64_t centiKmh);

// lap / sector / lap_ms of the row from the lap timer after it was fed this row's fix (tUs):
// lap = currentLapNo(), sector = sectorsDone() + 1 while timing (else 0), lap_ms = elapsed ms
// (floor) while timing (else empty). The replay mode compares its own timer with the file this way.
void setLapFields(MainRow* r, const LapTimer& t, int64_t tUs);

// ---- laps: <id>_<cid>_laps.csv -------------------------------------------------------------------
extern const char kLapsHeader[];  // "lap,start_utc_ms,end_utc_ms,lap_ms,sectors_ms\n"
int lapsHeader(char* buf, int cap);
// sectors_ms: the sector times joined with ';' (numSectors 0: empty field)
int lapsLine(char* buf, int cap, int lap, int64_t startUtcMs, int64_t endUtcMs, int64_t lapMs,
             const int64_t* sectorMs, int numSectors);
// From the lap timer (its tUs are UTC microseconds, as given by fixOf); times rounded to 1 ms
int lapsLine(char* buf, int cap, const LapResult& r);

// ---- IMU: <id>_<cid>_imu.csv (200 Hz, board frame) -------------------------------------------------
extern const char kImuHeader[];  // "t_ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps\n"
struct ImuRow {
  int64_t tMs;         // same clock as utc_ms of the main log
  float ax, ay, az;    // [g], 4 decimals
  float gx, gy, gz;    // [dps], 2 decimals
};
int imuHeader(char* buf, int cap);
int imuLine(char* buf, int cap, const ImuRow& r);

// ---- raw CAN: <id>_<cid>_can.csv ---------------------------------------------------------------------
extern const char kCanHeader[];  // "t_ms,bus,id,dlc,data\n"
int canHeader(char* buf, int cap);
// bus 'C' or 'B'; id as 8 hex digits; data = dlc bytes (at most 8) as hex without spaces: upper case
int canLine(char* buf, int cap, int64_t tMs, char bus, uint32_t id, int dlc, const uint8_t* data);

// ---- meta.json (ARCH §5.3; written at open, at every lap and at close) --------------------------------
struct Meta {
  const char* id = "";             // session id
  const char* fw = "";             // firmware version
  int tzMin = 540;
  int64_t startUtcMs = 0;
  int64_t endUtcMs = 0;
  const char* courseId = nullptr;  // null or "": "course": null (files use "street")
  const char* courseName = nullptr;
  int laps = 0;                    // completed laps
  int bestLap = 0;                 // 0: no lap yet -> best_lap / best_ms null
  int64_t bestMs = 0;
  bool maxKmhOk = false;
  float maxKmh = 0.0f;             // 1 decimal; not ok: null
  bool maxRpmOk = false;
  int maxRpm = 0;
  bool closed = false;
};
int metaJson(char* buf, int cap, const Meta& m);

// ---- names ----------------------------------------------------------------------------------------------
// Session id: local start time "YYYYMMDD_HHMMSS" (utcMs + tzMin)
int sessionId(char* buf, int cap, int64_t utcMs, int tzMin);
enum class FileKind : uint8_t { Csv, Vbo, Ubx, Imu, Can, Laps };
// "<id>_<cid>.csv", ".vbo", "_raw.ubx", "_imu.csv", "_can.csv", "_laps.csv"; cid null or "": "street"
int fileName(char* buf, int cap, const char* id, const char* cid, FileKind kind);

}  // namespace csvlog
}  // namespace lt
