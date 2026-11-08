#include "rcdiy.h"

#include <cmath>
#include <cstring>

namespace lt {
namespace rcdiy {

namespace {

inline int32_t clampI(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline void putLe16(uint8_t* p, int16_t v) {
  const uint16_t u = static_cast<uint16_t>(v);
  p[0] = static_cast<uint8_t>(u);
  p[1] = static_cast<uint8_t>(u >> 8);
}

}  // namespace

uint32_t dateHourCode(const GnssFix& f) {
  int y = static_cast<int>(f.year) - 2000;
  if (y < 0) y = 0;
  const int mo = f.month ? f.month - 1 : 0, d = f.day ? f.day - 1 : 0;
  return static_cast<uint32_t>(y * 8928 + mo * 744 + d * 24 + f.hour) & 0x1FFFFF;
}

void encodeGpsMain(const GnssFix& f, uint8_t sync, uint8_t out[20]) {
  // time since the full hour = min * 30000 + sec * 500 + ms / 2 (21 bits)
  int32_t ms = static_cast<int32_t>(f.min) * 60000 + static_cast<int32_t>(f.sec) * 1000 + f.nano / 1000000;
  if (ms < 0) ms = 0;
  if (ms > 3599999) ms = 3599999;
  const uint32_t t = static_cast<uint32_t>(ms / 2) & 0x1FFFFF;
  out[0] = static_cast<uint8_t>(((sync & 7) << 5) | ((t >> 16) & 0x1F));
  out[1] = static_cast<uint8_t>(t >> 8);
  out[2] = static_cast<uint8_t>(t);
  // fix quality as NMEA GGA (0 none, 1 GPS, 2 differential) and satellites
  const uint8_t q = f.fixOk ? (f.diffSoln ? 2 : 1) : 0;
  const uint8_t sv = f.numSV > 0x3E ? 0x3E : f.numSV;
  out[3] = static_cast<uint8_t>((q << 6) | (sv & 0x3F));
  const uint32_t lat = f.fixOk ? static_cast<uint32_t>(f.lat) : 0x7FFFFFFFu;
  const uint32_t lon = f.fixOk ? static_cast<uint32_t>(f.lon) : 0x7FFFFFFFu;
  out[4] = static_cast<uint8_t>(lat >> 24);
  out[5] = static_cast<uint8_t>(lat >> 16);
  out[6] = static_cast<uint8_t>(lat >> 8);
  out[7] = static_cast<uint8_t>(lat);
  out[8] = static_cast<uint8_t>(lon >> 24);
  out[9] = static_cast<uint8_t>(lon >> 16);
  out[10] = static_cast<uint8_t>(lon >> 8);
  out[11] = static_cast<uint8_t>(lon);
  uint32_t alt = 0xFFFF, spd = 0xFFFF, brg = 0xFFFF;
  if (f.fixOk) {
    const float m = static_cast<float>(f.hMSL) / 1000.0f;
    if (m > 2776.7f) {
      alt = (static_cast<uint32_t>(clampI(static_cast<int32_t>(std::lround(m + 500.0f)), 0, 0x7FFF)) & 0x7FFF) | 0x8000;
    } else {
      alt = static_cast<uint32_t>(clampI(static_cast<int32_t>(std::lround((m + 500.0f) * 10.0f)), 0, 0x7FFF)) & 0x7FFF;
    }
    const float kmh = static_cast<float>(f.gSpeed) * 0.0036f;
    if (kmh > 327.67f) {
      spd = (static_cast<uint32_t>(clampI(static_cast<int32_t>(std::lround(kmh * 10.0f)), 0, 0x7FFF)) & 0x7FFF) | 0x8000;
    } else {
      spd = static_cast<uint32_t>(clampI(static_cast<int32_t>(std::lround(kmh * 100.0f)), 0, 0x7FFF)) & 0x7FFF;
    }
    int32_t deg100 = f.headMot / 1000;  // 1e-5 deg -> 0.01 deg
    while (deg100 < 0) deg100 += 36000;
    while (deg100 >= 36000) deg100 -= 36000;
    brg = static_cast<uint32_t>(deg100);
  }
  out[12] = static_cast<uint8_t>(alt >> 8);
  out[13] = static_cast<uint8_t>(alt);
  out[14] = static_cast<uint8_t>(spd >> 8);
  out[15] = static_cast<uint8_t>(spd);
  out[16] = static_cast<uint8_t>(brg >> 8);
  out[17] = static_cast<uint8_t>(brg);
  // HDOP field gets pDOP (NAV-PVT has no hDOP; pDOP >= hDOP, so it is a cautious value)
  const uint32_t dop = f.fixOk ? static_cast<uint32_t>(clampI((f.pDOP + 5) / 10, 0, 0xFE)) : 0xFF;
  out[18] = static_cast<uint8_t>(dop);
  out[19] = 0xFF;  // VDOP: none
}

void encodeGpsTime(const GnssFix& f, uint8_t sync, uint8_t out[3]) {
  const uint32_t c = dateHourCode(f);
  out[0] = static_cast<uint8_t>(((sync & 7) << 5) | ((c >> 16) & 0x1F));
  out[1] = static_cast<uint8_t>(c >> 8);
  out[2] = static_cast<uint8_t>(c);
}

size_t encodeCan(uint32_t id, const uint8_t* d, uint8_t dlc, uint8_t out[20]) {
  if (dlc > 16) dlc = 16;
  out[0] = static_cast<uint8_t>(id);
  out[1] = static_cast<uint8_t>(id >> 8);
  out[2] = static_cast<uint8_t>(id >> 16);
  out[3] = static_cast<uint8_t>(id >> 24);
  if (dlc) std::memcpy(out + 4, d, dlc);
  return 4 + static_cast<size_t>(dlc);
}

int16_t clampI16(float v) {
  if (v > 32767.0f) return 32767;
  if (v < -32768.0f) return -32768;
  if (!(v == v)) return 0;  // NaN
  return static_cast<int16_t>(std::lround(v));
}

size_t encodeImuAccel(float lonG, float latG, float vertG, uint8_t out[20]) {
  uint8_t p[6];
  putLe16(p, clampI16(lonG * 1000.0f));
  putLe16(p + 2, clampI16(latG * 1000.0f));
  putLe16(p + 4, clampI16(vertG * 1000.0f));
  return encodeCan(RC_PID_IMU_ACCEL, p, 6, out);
}

size_t encodeImuGyro(float yawDps, uint8_t out[20]) {
  uint8_t p[6] = {0, 0, 0, 0, 0, 0};
  putLe16(p, clampI16(yawDps * 100.0f));
  return encodeCan(RC_PID_IMU_GYRO, p, 6, out);
}

// ---- PID filter

void Filter::reset() {
  allowAll_ = false;
  allInterval_ = 0;
  n_ = 0;
  allN_ = 0;
}

void Filter::command(const uint8_t* data, size_t len) {
  if (len < 1) return;
  const uint8_t cmd = data[0];
  if (cmd == 0) {  // deny all
    allowAll_ = false;
    n_ = 0;
  } else if (cmd == 1 && len >= 3) {  // allow all, interval
    allowAll_ = true;
    allInterval_ = static_cast<uint16_t>((data[1] << 8) | data[2]);
    n_ = 0;
    allN_ = 0;
  } else if (cmd == 2 && len >= 7) {  // allow this PID, interval
    const uint16_t iv = static_cast<uint16_t>((data[1] << 8) | data[2]);
    const uint32_t pid = (static_cast<uint32_t>(data[3]) << 24) | (static_cast<uint32_t>(data[4]) << 16) |
                         (static_cast<uint32_t>(data[5]) << 8) | data[6];
    for (int i = 0; i < n_; i++) {
      if (pid_[i] == pid) {
        interval_[i] = iv;
        return;
      }
    }
    if (n_ < MAX_PIDS) {
      pid_[n_] = pid;
      interval_[n_] = iv;
      lastMs_[n_] = 0;
      n_++;
    }
  }
}

bool Filter::pass(uint32_t pid, uint32_t nowMs, uint16_t minMs) {
  if (allowAll_) {
    const uint16_t iv = allInterval_ > minMs ? allInterval_ : minMs;
    if (iv == 0) return true;
    for (int i = 0; i < allN_; i++) {
      if (allPid_[i] == pid) {
        if (static_cast<uint32_t>(nowMs - allMs_[i]) < iv) return false;
        allMs_[i] = nowMs;
        return true;
      }
    }
    if (allN_ < MAX_PIDS) {
      allPid_[allN_] = pid;
      allMs_[allN_] = nowMs;
      allN_++;
    }
    return true;
  }
  for (int i = 0; i < n_; i++) {
    if (pid_[i] != pid) continue;
    const uint16_t iv = interval_[i] > minMs ? interval_[i] : minMs;
    if (iv && lastMs_[i] && static_cast<uint32_t>(nowMs - lastMs_[i]) < iv) return false;
    lastMs_[i] = nowMs;
    return true;
  }
  return false;
}

bool Filter::allowed(uint32_t pid) const {
  if (allowAll_) return true;
  for (int i = 0; i < n_; i++) {
    if (pid_[i] == pid) return true;
  }
  return false;
}

// ---- GPS time schedule

bool GpsTimeSync::update(const GnssFix& f, uint32_t nowMs) {
  const uint32_t code = dateHourCode(f);
  const bool changed = code != prevCode_;
  if (changed) {
    prevCode_ = code;
    sync_ = static_cast<uint8_t>((sync_ + 1) & 7);
  }
  if (changed || static_cast<uint32_t>(nowMs - timeMs_) >= 1000) {
    timeMs_ = nowMs;
    return true;
  }
  return false;
}

}  // namespace rcdiy
}  // namespace lt
