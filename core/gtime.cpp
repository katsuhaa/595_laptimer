#include "gtime.h"

#include <cstdlib>
#include <cstring>

namespace lt {
namespace gtime {

namespace {

inline int64_t floorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

void put2(char* p, unsigned v) {
  p[0] = static_cast<char>('0' + (v / 10) % 10);
  p[1] = static_cast<char>('0' + v % 10);
}

}  // namespace

uint32_t epoch(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t min, uint8_t sec) {
  // Days since 1970-01-01 (Gregorian, years starting in March)
  const int y = static_cast<int>(year) - (month <= 2 ? 1 : 0);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy =
      (153u * static_cast<unsigned>(month > 2 ? month - 3 : month + 9) + 2u) / 5u + day - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  const int64_t days = static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
  return static_cast<uint32_t>(days * 86400 + static_cast<int64_t>(hour) * 3600 +
                               static_cast<int64_t>(min) * 60 + sec);
}

void toLocal(int64_t utcSec, int tzMin, DateTime* out) {
  const int64_t t = utcSec + static_cast<int64_t>(tzMin) * 60;
  const int64_t days = floorDiv(t, 86400);
  const int64_t rem = t - days * 86400;
  // civil date from days since 1970-01-01 (inverse of epoch())
  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  const int64_t y = static_cast<int64_t>(yoe) + era * 400 + (m <= 2 ? 1 : 0);
  out->year = static_cast<uint16_t>(y);
  out->month = static_cast<uint8_t>(m);
  out->day = static_cast<uint8_t>(d);
  out->hour = static_cast<uint8_t>(rem / 3600);
  out->min = static_cast<uint8_t>(rem / 60 % 60);
  out->sec = static_cast<uint8_t>(rem % 60);
  const int64_t w = (days + 4) % 7;  // 1970-01-01 was a Thursday
  out->wday = static_cast<uint8_t>(w < 0 ? w + 7 : w);
}

size_t formatId(const DateTime& t, char* out, size_t cap) {
  if (!out || cap < 16) return 0;
  const unsigned y = t.year;
  out[0] = static_cast<char>('0' + (y / 1000) % 10);
  out[1] = static_cast<char>('0' + (y / 100) % 10);
  put2(out + 2, y % 100);
  put2(out + 4, t.month);
  put2(out + 6, t.day);
  out[8] = '_';
  put2(out + 9, t.hour);
  put2(out + 11, t.min);
  put2(out + 13, t.sec);
  out[15] = 0;
  return 15;
}

uint32_t demoBase(const char* date, const char* time, int tzMin) {
  static const char kMon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  if (!date || !time || std::strlen(date) != 11 || std::strlen(time) != 8 || time[2] != ':' ||
      time[5] != ':') {
    return 0;
  }
  int mon = 0;
  for (int i = 0; i < 12; i++) {
    if (std::strncmp(date, kMon + i * 3, 3) == 0) mon = i + 1;
  }
  const int day = std::atoi(date + 4), year = std::atoi(date + 7), hh = std::atoi(time),
            mm = std::atoi(time + 3), ss = std::atoi(time + 6);
  if (mon == 0 || day < 1 || day > 31 || year < 2020 || year > 2099 || hh > 23 || mm > 59 || ss > 59) {
    return 0;
  }
  const int64_t local = epoch(static_cast<uint16_t>(year), static_cast<uint8_t>(mon),
                              static_cast<uint8_t>(day), static_cast<uint8_t>(hh),
                              static_cast<uint8_t>(mm), static_cast<uint8_t>(ss));
  return static_cast<uint32_t>(local - static_cast<int64_t>(tzMin) * 60);
}

void Clock::onFix(const GnssFix& f) {
  if (!f.timeOk || f.year < 2020 || f.month < 1 || f.month > 12 || f.day < 1 || f.day > 31 ||
      f.hour > 23 || f.min > 59 || f.sec > 60) {
    return;
  }
  utc_ = epoch(f.year, f.month, f.day, f.hour, f.min, f.sec);
  // millis at the start of that second (nano can be negative; the UART delay is not corrected)
  ms_ = f.rxMs - static_cast<uint32_t>(f.nano / 1000000);
  ok_ = true;
}

bool Clock::now(uint32_t nowMs, uint32_t* utc) const {
  if (!ok_) return false;
  const int32_t d = static_cast<int32_t>(nowMs - ms_);
  *utc = d >= 0 ? utc_ + static_cast<uint32_t>(d) / 1000u
                : utc_ - (static_cast<uint32_t>(-static_cast<int64_t>(d)) + 999u) / 1000u;
  return true;
}

bool Clock::nowUtcMs(uint32_t nowMs, int64_t* utcMs) const {
  if (!ok_) return false;
  const int32_t d = static_cast<int32_t>(nowMs - ms_);
  *utcMs = static_cast<int64_t>(utc_) * 1000 + d;
  return true;
}

}  // namespace gtime
}  // namespace lt
