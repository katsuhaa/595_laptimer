// rtc.cpp - see rtc.h
#include "rtc.h"

#include "touch.h"

namespace hw {
namespace {

constexpr uint8_t kAddr = 0x51;     // demo PCF85063_register_read/write(0x51, ...)
constexpr uint8_t kTimeReg = 0x04;  // seconds register; 7 registers up to years
constexpr uint32_t kUtc2000 = 946684800u;   // 2000-01-01 00:00:00 UTC
constexpr uint32_t kUtc2100 = 4102444800u;  // 2100-01-01 00:00:00 UTC

// demo PCD85063TP_decToBcd / PCD85063TP_bcdToDec
uint8_t toBcd(unsigned v) {
  return (uint8_t)((v / 10) * 16 + (v % 10));
}
bool bcdOk(uint8_t b) {
  return (b & 0x0F) <= 9 && (b >> 4) <= 9;
}
unsigned fromBcd(uint8_t b) {
  return (unsigned)((b >> 4) * 10 + (b & 0x0F));
}

// demo PCF85063_get_Week(): 0 = Sunday
int weekDay(int y, int m, int d) {
  if (m == 1 || m == 2) {
    m += 12;
    y--;
  }
  const int week = (d + 2 * m + 3 * (m + 1) / 5 + y + y / 4 - y / 100 + y / 400) % 7;
  return (week + 1) % 7;
}

// Civil date <-> days since 1970-01-01 (proleptic Gregorian; H. Hinnant's days_from_civil / civil_from_days)
int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2 ? 1 : 0;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = (uint32_t)(y - era * 400);
  const uint32_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

void civilFromDays(int32_t z, int* y, unsigned* m, unsigned* d) {
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = (uint32_t)(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = (int)yoe + era * 400 + (*m <= 2 ? 1 : 0);
}

unsigned daysInMonth(int y, unsigned m) {
  static const uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
  return kDays[m - 1];
}

// Decodes the 7 registers as the demo PCF85063_getTime() (masks 0x7F, 0x7F, 0x3F, 0x3F, -, 0x1F; month bit 7 = 19xx)
bool decode(const uint8_t r[7], uint32_t* utc) {
  const uint8_t sec = r[0] & 0x7F, min = r[1] & 0x7F, hour = r[2] & 0x3F, day = r[3] & 0x3F, mon = r[5] & 0x1F;
  const uint8_t yr = r[6];
  if (!bcdOk(sec) || !bcdOk(min) || !bcdOk(hour) || !bcdOk(day) || !bcdOk(mon) || !bcdOk(yr)) return false;
  if (r[5] & 0x80) return false;  // demo's 19xx flag: before 2000, never written by rtcSet
  const int y = 2000 + (int)fromBcd(yr);
  const unsigned m = fromBcd(mon), d = fromBcd(day), hh = fromBcd(hour), mm = fromBcd(min), ss = fromBcd(sec);
  if (m < 1 || m > 12 || d < 1 || d > daysInMonth(y, m) || hh > 23 || mm > 59 || ss > 59) return false;
  const int64_t t = (int64_t)daysFromCivil(y, m, d) * 86400 + hh * 3600 + mm * 60 + ss;
  *utc = (uint32_t)t;
  return true;
}

bool readRegs(uint8_t r[7]) {
  return i2cReadReg(kAddr, kTimeReg, r, 7);
}

}  // namespace

bool rtcBegin() {
  uint8_t r[7];
  return readRegs(r);
}

bool rtcGet(uint32_t* utc, bool* oscStopped) {
  uint8_t r[7];
  if (!readRegs(r)) return false;
  const bool os = (r[0] & 0x80) != 0;
  if (oscStopped) *oscStopped = os;
  if (os) return false;
  uint32_t t = 0;
  if (!decode(r, &t)) return false;
  if (utc) *utc = t;
  return true;
}

bool rtcSet(uint32_t utc) {
  if (utc < kUtc2000 || utc >= kUtc2100) return false;
  int y;
  unsigned m, d;
  const uint32_t days = utc / 86400u, sod = utc % 86400u;
  civilFromDays((int32_t)days, &y, &m, &d);
  // demo PCF85063_setTime(tm, isAutoCalcWeek = 1); seconds bit 7 (OS) written as 0 = cleared
  uint8_t r[7];
  r[0] = toBcd(sod % 60);
  r[1] = toBcd(sod / 60 % 60);
  r[2] = toBcd(sod / 3600);
  r[3] = toBcd(d);
  r[4] = toBcd((unsigned)weekDay(y, (int)m, (int)d));
  r[5] = toBcd(m);  // years >= 2000: no 19xx flag
  r[6] = toBcd((unsigned)(y - 2000));
  if (!i2cWriteReg(kAddr, kTimeReg, r, sizeof r)) return false;
  uint32_t back = 0;
  if (!rtcGet(&back)) return false;
  const int64_t diff = (int64_t)back - (int64_t)utc;
  return diff >= -2 && diff <= 2;
}

}  // namespace hw
