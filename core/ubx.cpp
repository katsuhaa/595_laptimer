#include "ubx.h"

#include <cstring>

#include "gtime.h"

namespace lt {

Fix toLapFix(const GnssFix& g, const LapFixRules& rules) {
  Fix f;
  f.tUs = g.utcUs;
  f.latE7 = g.lat;
  f.lonE7 = g.lon;
  f.speedMps = static_cast<float>(g.gSpeed) / 1000.0f;
  f.valid = g.fixOk && g.fixType >= rules.minFixType && g.timeOk &&
            (rules.maxHAccMm == 0 || g.hAcc_mm <= rules.maxHAccMm);
  return f;
}

namespace ubx {

namespace {

constexpr uint32_t kWeekMs = 604800000u;

inline uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline int32_t i32(const uint8_t* p) { return static_cast<int32_t>(u32(p)); }

// floor(a / b) for b > 0
inline int64_t floorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

void copyStr(char* dst, size_t cap, const char* src) {
  if (!dst || !cap) return;
  size_t i = 0;
  for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
  dst[i] = 0;
}

// Fixed-width text field (NUL padded) to a C string
void field(char* dst, size_t dstSize, const uint8_t* src, size_t width) {
  size_t i = 0;
  for (; i < width && i + 1 < dstSize && src[i]; i++) dst[i] = static_cast<char>(src[i]);
  dst[i] = 0;
}

int hexVal(uint8_t b) {
  if (b >= '0' && b <= '9') return b - '0';
  if (b >= 'A' && b <= 'F') return b - 'A' + 10;
  if (b >= 'a' && b <= 'f') return b - 'a' + 10;
  return -1;
}

// Adds key + value to a CFG-VALSET payload. Value size from bits 28..30 of the key:
// 1 = one bit (1 byte), 2 = 1 byte, 3 = 2 bytes, 4 = 4 bytes, 5 = 8 bytes
size_t putKey(uint8_t* p, uint32_t key, uint32_t val) {
  p[0] = static_cast<uint8_t>(key);
  p[1] = static_cast<uint8_t>(key >> 8);
  p[2] = static_cast<uint8_t>(key >> 16);
  p[3] = static_cast<uint8_t>(key >> 24);
  const int size = static_cast<int>((key >> 28) & 7);
  const int n = (size == 3) ? 2 : (size == 4) ? 4 : (size == 5) ? 8 : 1;
  for (int i = 0; i < n; i++) p[4 + i] = i < 4 ? static_cast<uint8_t>(val >> (8 * i)) : 0;
  return 4 + static_cast<size_t>(n);
}

size_t valsetHead(uint8_t* pay) {
  pay[0] = 0x00;       // version 0
  pay[1] = LAYER_RAM;  // RAM only: the firmware configures the receiver at every start
  pay[2] = 0x00;       // reserved
  pay[3] = 0x00;
  return 4;
}

}  // namespace

void Parser::begin() {
  c_ = Counters{};
  state_ = 0;
  cls_ = id_ = ckA_ = ckB_ = 0;
  plen_ = pos_ = 0;
  last_ = GnssFix{};
  have_ = false;
  qHead_ = qN_ = 0;
  epochRef_ = false;
  prevItow_ = prevRxMs_ = 0;
  monSw_[0] = monHw_[0] = 0;
  monExtN_ = 0;
  nmeaState_ = 0;
  nmeaXor_ = nmeaCk_ = 0;
  nmeaLen_ = nmeaAddr_ = 0;
  nmeaInAddr_ = false;
  tickMs_ = tickPvt_ = tickBytes_ = 0;
  tickStarted_ = false;
}

void Parser::setEpochPeriodMs(uint32_t ms) {
  periodMs_ = ms;
  epochRef_ = false;
}

// NAV-PVT payload (u-blox interface description; same layout as SparkFun UBX_NAV_PVT_data_t)
void Parser::decodePvt(const uint8_t* d, uint32_t nowMs) {
  GnssFix f{};
  f.iTOW = u32(d + 0);
  f.year = u16(d + 4);
  f.month = d[6];
  f.day = d[7];
  f.hour = d[8];
  f.min = d[9];
  f.sec = d[10];
  const uint8_t valid = d[11];
  f.timeOk = (valid & 0x03) == 0x03;  // validDate && validTime
  f.timeResolved = (valid & 0x04) != 0;
  f.nano = i32(d + 16);
  f.fixType = d[20];
  const uint8_t flags = d[21];
  f.fixOk = (flags & 0x01) && (f.fixType == 2 || f.fixType == 3 || f.fixType == 4);
  f.diffSoln = (flags & 0x02) != 0;
  f.numSV = d[23];
  f.lon = i32(d + 24);
  f.lat = i32(d + 28);
  f.hMSL = i32(d + 36);
  f.hAcc_mm = u32(d + 40);
  f.vAcc_mm = u32(d + 44);
  f.gSpeed = i32(d + 60);
  f.headMot = i32(d + 64);
  f.sAcc_mmps = u32(d + 68);
  f.headAcc = u32(d + 72);
  f.pDOP = u16(d + 76);
  f.rxMs = nowMs;
  if (f.timeOk) {
    const int64_t s = gtime::epoch(f.year, f.month, f.day, f.hour, f.min, f.sec);
    f.utcUs = s * 1000000 + floorDiv(f.nano, 1000);
  }
  countEpoch(f);
  last_ = f;
  have_ = true;
  c_.pvt++;
  if (qN_ == kFixQueue) {  // full: drop the oldest
    qHead_ = (qHead_ + 1) % kFixQueue;
    qN_--;
    c_.fixDropped++;
  }
  q_[(qHead_ + qN_) % kFixQueue] = f;
  qN_++;
}

// Missing epochs from the iTOW step (week wrap handled). When the step disagrees with the local clock
// by more than 1 s (receiver restart, time correction), the local elapsed time is used instead.
void Parser::countEpoch(const GnssFix& f) {
  if (periodMs_ == 0) return;
  if (!epochRef_) {
    epochRef_ = true;
    prevItow_ = f.iTOW;
    prevRxMs_ = f.rxMs;
    return;
  }
  uint32_t step = (f.iTOW + kWeekMs - prevItow_) % kWeekMs;
  if (step == 0) {
    c_.dupEpochs++;
    return;
  }
  const uint32_t local = f.rxMs - prevRxMs_;
  const uint32_t diff = step > local ? step - local : local - step;
  if (diff > 1000) {
    c_.timeJumps++;
    step = local;
  }
  prevItow_ = f.iTOW;
  prevRxMs_ = f.rxMs;
  if (step > c_.maxGapMs) c_.maxGapMs = step;
  const uint32_t epochs = (step + periodMs_ / 2) / periodMs_;  // nearest whole number of periods
  if (epochs >= 2) {
    c_.missedEpochs += epochs - 1;
    c_.gaps++;
  }
}

void Parser::dispatch(uint32_t nowMs) {
  c_.frames++;
  const size_t stored = plen_ < kPayloadCap ? plen_ : static_cast<size_t>(kPayloadCap);
  if (cls_ == CLS_NAV && id_ == ID_NAV_PVT && plen_ == NAV_PVT_LEN) {
    decodePvt(pay_, nowMs);
  } else if (cls_ == CLS_ACK && (id_ == ID_ACK_ACK || id_ == ID_ACK_NAK)) {
    const bool ack = id_ == ID_ACK_ACK;
    if (ack) {
      c_.ack++;
    } else {
      c_.nak++;
    }
    if (plen_ == 2 && pay_[0] == CLS_CFG && pay_[1] == ID_CFG_VALSET) {
      if (ack) {
        c_.ackValset++;
      } else {
        c_.nakValset++;
      }
    }
  } else if (cls_ == CLS_MON && id_ == ID_MON_VER && plen_ >= 40) {
    field(monSw_, sizeof(monSw_), pay_, 30);
    field(monHw_, sizeof(monHw_), pay_ + 30, 10);
    int n = static_cast<int>((stored - 40) / 30);
    if (n > kMonVerExtMax) n = kMonVerExtMax;
    for (int i = 0; i < n; i++) field(monExt_[i], sizeof(monExt_[i]), pay_ + 40 + 30 * i, 30);
    monExtN_ = n;
    c_.monVer++;
  }
}

// NMEA sentence check, independent of the UBX state: $ + address (5+ of A-Z 0-9) + ... + *HH CR LF
void Parser::nmeaByte(uint8_t b) {
  if (b == '$') {
    nmeaState_ = 1;
    nmeaXor_ = 0;
    nmeaLen_ = 0;
    nmeaAddr_ = 0;
    nmeaInAddr_ = true;
    return;
  }
  switch (nmeaState_) {
    case 1:
      if (b == '*') {
        nmeaState_ = (nmeaAddr_ >= 5 && !nmeaInAddr_) ? 2 : 0;
      } else if (b < 0x20 || b > 0x7E || ++nmeaLen_ > 90) {
        nmeaState_ = 0;
      } else {
        nmeaXor_ = static_cast<uint8_t>(nmeaXor_ ^ b);
        if (nmeaInAddr_) {
          if (b == ',') {
            nmeaInAddr_ = false;
          } else if ((b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9')) {
            nmeaAddr_++;
          } else {
            nmeaState_ = 0;
          }
        }
      }
      break;
    case 2: {
      const int v = hexVal(b);
      if (v < 0) {
        nmeaState_ = 0;
      } else {
        nmeaCk_ = static_cast<uint8_t>(v << 4);
        nmeaState_ = 3;
      }
      break;
    }
    case 3: {
      const int v = hexVal(b);
      nmeaState_ = (v >= 0 && static_cast<uint8_t>(nmeaCk_ | v) == nmeaXor_) ? 4 : 0;
      break;
    }
    case 4:
      nmeaState_ = b == '\r' ? 5 : 0;
      break;
    case 5:
      if (b == '\n') c_.nmea++;
      nmeaState_ = 0;
      break;
    default:
      nmeaState_ = 0;
      break;
  }
}

int Parser::feed(const uint8_t* data, size_t n, uint32_t nowMs) {
  int got = 0;
  c_.bytes += static_cast<uint32_t>(n);
  for (size_t i = 0; i < n; i++) {
    const uint8_t b = data[i];
    nmeaByte(b);
    switch (state_) {
      case 0:
        if (b == 0xB5) state_ = 1;
        break;
      case 1:
        state_ = (b == 0x62) ? 2 : (b == 0xB5 ? 1 : 0);
        break;
      case 2:
        cls_ = b;
        ckA_ = b;
        ckB_ = b;
        state_ = 3;
        break;
      case 3:
        id_ = b;
        ckA_ = static_cast<uint8_t>(ckA_ + b);
        ckB_ = static_cast<uint8_t>(ckB_ + ckA_);
        state_ = 4;
        break;
      case 4:
        plen_ = b;
        ckA_ = static_cast<uint8_t>(ckA_ + b);
        ckB_ = static_cast<uint8_t>(ckB_ + ckA_);
        state_ = 5;
        break;
      case 5:
        plen_ = static_cast<uint16_t>(plen_ | (b << 8));
        ckA_ = static_cast<uint8_t>(ckA_ + b);
        ckB_ = static_cast<uint8_t>(ckB_ + ckA_);
        pos_ = 0;
        if (plen_ > MAX_SKIP) {
          state_ = 0;  // impossible length: resync
        } else {
          state_ = plen_ ? 6 : 7;
        }
        break;
      case 6:
        if (pos_ < kPayloadCap) pay_[pos_] = b;
        pos_++;
        ckA_ = static_cast<uint8_t>(ckA_ + b);
        ckB_ = static_cast<uint8_t>(ckB_ + ckA_);
        if (pos_ >= plen_) state_ = 7;
        break;
      case 7:
        state_ = (b == ckA_) ? 8 : 9;
        break;
      case 8:
      case 9:
        if (state_ == 8 && b == ckB_) {
          const uint32_t pvt0 = c_.pvt;
          dispatch(nowMs);
          if (c_.pvt != pvt0) got++;
        } else {
          c_.bad++;
        }
        state_ = 0;
        break;
      default:
        state_ = 0;
        break;
    }
  }
  return got;
}

bool Parser::takeFix(GnssFix* out) {
  if (qN_ == 0) return false;
  *out = q_[qHead_];
  qHead_ = (qHead_ + 1) % kFixQueue;
  qN_--;
  return true;
}

bool Parser::latest(GnssFix* out) const {
  if (!have_) return false;
  *out = last_;
  return true;
}

void Parser::tick(uint32_t nowMs) {
  if (!tickStarted_) {
    tickStarted_ = true;
    tickMs_ = nowMs;
    tickPvt_ = c_.pvt;
    tickBytes_ = c_.bytes;
    return;
  }
  if (static_cast<uint32_t>(nowMs - tickMs_) < 1000) return;
  tickMs_ = nowMs;
  c_.pvtPerSec = c_.pvt - tickPvt_;
  tickPvt_ = c_.pvt;
  c_.bytesPerSec = c_.bytes - tickBytes_;
  tickBytes_ = c_.bytes;
}

bool Parser::monVer(char* sw, size_t swCap, char* hw, size_t hwCap) const {
  if (!c_.monVer) return false;
  copyStr(sw, swCap, monSw_);
  copyStr(hw, hwCap, monHw_);
  return true;
}

bool Parser::monVerExt(int i, char* out, size_t cap) const {
  if (i < 0 || i >= monExtN_) return false;
  copyStr(out, cap, monExt_[i]);
  return true;
}

bool Parser::monVerFind(const char* prefix, char* out, size_t cap) const {
  const size_t n = std::strlen(prefix);
  for (int i = 0; i < monExtN_; i++) {
    if (std::strncmp(monExt_[i], prefix, n) == 0) {
      copyStr(out, cap, monExt_[i] + n);
      return true;
    }
  }
  return false;
}

size_t frame(uint8_t* out, size_t cap, uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len) {
  const size_t total = static_cast<size_t>(len) + 8;
  if (!out || cap < total) return 0;
  out[0] = 0xB5;
  out[1] = 0x62;
  out[2] = cls;
  out[3] = id;
  out[4] = static_cast<uint8_t>(len & 0xFF);
  out[5] = static_cast<uint8_t>(len >> 8);
  if (len) std::memcpy(out + 6, payload, len);
  uint8_t a = 0, b = 0;
  for (size_t i = 2; i < 6 + static_cast<size_t>(len); i++) {
    a = static_cast<uint8_t>(a + out[i]);
    b = static_cast<uint8_t>(b + a);
  }
  out[6 + len] = a;
  out[7 + len] = b;
  return total;
}

size_t buildBaud(uint8_t* out, size_t cap, uint32_t baud) {
  uint8_t pay[16];
  size_t n = valsetHead(pay);
  n += putKey(pay + n, KEY_UART1_BAUD, baud);
  return frame(out, cap, CLS_CFG, ID_CFG_VALSET, pay, static_cast<uint16_t>(n));
}

size_t buildNavConfig(uint8_t* out, size_t cap, int rateHz) {
  uint8_t pay[64];
  size_t n = valsetHead(pay);
  int meas = rateHz > 0 ? 1000 / rateHz : 1000;
  if (meas < 40) meas = 40;  // 25 Hz is the NEO-M9N maximum (and the round gauge's minimum)
  n += putKey(pay + n, KEY_RATE_MEAS, static_cast<uint32_t>(meas));
  n += putKey(pay + n, KEY_RATE_NAV, 1);
  n += putKey(pay + n, KEY_MSGOUT_PVT_UART1, 1);
  n += putKey(pay + n, KEY_UART1OUT_UBX, 1);
  n += putKey(pay + n, KEY_UART1OUT_NMEA, 0);
  n += putKey(pay + n, KEY_UART1IN_UBX, 1);
  n += putKey(pay + n, KEY_DYNMODEL, DYNMODEL_AUTOMOTIVE);
  return frame(out, cap, CLS_CFG, ID_CFG_VALSET, pay, static_cast<uint16_t>(n));
}

size_t buildPollMonVer(uint8_t* out, size_t cap) { return frame(out, cap, CLS_MON, ID_MON_VER, nullptr, 0); }

}  // namespace ubx
}  // namespace lt
