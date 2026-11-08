// core/gtime.h - current time from the GNSS. Port of the round gauge's gtime
// (katsuhaa/abarth595-round-gauge 05ab0a1, meter_display/app.cpp `gtime`): set from every NAV-PVT with
// a valid date and time, then advanced with millis, so it keeps going when the GNSS drops out.
// Added: local time (tz offset in minutes, default +540 = JST) as year..second for file names
// (session id YYYYMMDD_HHMMSS) and the clock.
#pragma once

#include <cstddef>
#include <cstdint>

#include "ubx.h"

namespace lt {
namespace gtime {

constexpr int kTzJstMin = 540;  // JST = UTC + 9 h (the round gauge's CLOCK_TZ_SEC)

// Date and time (UTC) to seconds since 1970-01-01 (Gregorian; valid 1970..2105)
uint32_t epoch(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t min, uint8_t sec);

struct DateTime {
  uint16_t year;
  uint8_t month, day, hour, min, sec;
  uint8_t wday;  // 0 = Sunday
};

// UTC seconds + tzMin minutes -> local date and time
void toLocal(int64_t utcSec, int tzMin, DateTime* out);
// "YYYYMMDD_HHMMSS" (15 chars + NUL; cap >= 16). Returns the length, 0 when cap is too small.
size_t formatId(const DateTime& t, char* out, size_t cap);

// Start of the demo clock from __DATE__ ("Oct 28 2026") and __TIME__ ("12:34:56") of the build PC in
// local time (tzMin). Returns UTC seconds, 0 when unreadable.
uint32_t demoBase(const char* date, const char* time, int tzMin = kTzJstMin);

class Clock {
 public:
  void reset() { ok_ = false; utc_ = 0; ms_ = 0; }
  // Every new NAV-PVT. Fixes without a usable date and time (timeOk, year >= 2020, fields in range)
  // are ignored.
  void onFix(const GnssFix& f);
  bool valid() const { return ok_; }
  // Start from another source (the RTC at power-up) until the first GNSS fix: utc = UTC seconds at nowMs
  void set(uint32_t utc, uint32_t nowMs) {
    utc_ = utc;
    ms_ = nowMs;
    ok_ = true;
  }
  // UTC seconds now (nowMs = millis). false until the first usable fix.
  bool now(uint32_t nowMs, uint32_t* utc) const;
  // UTC milliseconds now
  bool nowUtcMs(uint32_t nowMs, int64_t* utcMs) const;

 private:
  bool ok_ = false;
  uint32_t utc_ = 0;  // start of the second of the last fix
  uint32_t ms_ = 0;   // millis at that moment
};

}  // namespace gtime
}  // namespace lt
