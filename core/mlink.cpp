#include "mlink.h"

#include <cstring>

namespace lt {
namespace mlink {

namespace {
const uint32_t kBauds[] = METER_LINK_BAUD_CANDIDATES;
constexpr int kNumBauds = static_cast<int>(sizeof(kBauds) / sizeof(kBauds[0]));
}  // namespace

int Parser::candidates() { return kNumBauds; }

uint32_t Parser::candidate(int i) { return (i >= 0 && i < kNumBauds) ? kBauds[i] : kBauds[0]; }

void Parser::begin() {
  len_ = 0;
  std::memset(&sig_, 0, sizeof(sig_));
  sigSeen_ = false;
  sigMs_ = 0;
  std::memset(&st_, 0, sizeof(st_));
  infoMs_ = 0;
  haveSeq_ = false;
  lastSeq_ = 0;
  lastGoodMs_ = 0;
  anySeen_ = false;
  tickMs_ = 0;
  tickFrames_ = 0;
  tickCan_ = 0;
  rxBytes_ = 0;
  tickBytes_ = 0;
  graceOn_ = false;
  graceUntil_ = 0;
  abIdx_ = 0;
  abStarted_ = false;
  abWinMs_ = 0;
  abWinRx_ = 0;
  abWinGood_ = 0;
  abBadWins_ = 0;
  st_.baud = kBauds[0];
}

void Parser::graceUntil(uint32_t untilMs) {
  graceUntil_ = untilMs;
  graceOn_ = true;
}

bool Parser::inGrace(uint32_t nowMs) {
  if (!graceOn_) return false;
  if (static_cast<int32_t>(nowMs - graceUntil_) >= 0) {
    graceOn_ = false;
    return false;
  }
  return true;
}

// Type and length must fit the defined payloads
bool Parser::plausible(uint8_t type, uint8_t len, const uint8_t* payload) {
  switch (type) {
    case ML_TYPE_SIGNALS:
      return len == sizeof(MlSignals);
    case ML_TYPE_INFO:
      return len == sizeof(MlInfo);
    case ML_TYPE_CAN:
      return len >= sizeof(MlCanHead) && (payload[0] & 0x0F) <= 8 &&
             len == sizeof(MlCanHead) + (payload[0] & 0x0F);
    default:
      return false;
  }
}

void Parser::handle(uint8_t type, uint8_t seq, const uint8_t* p, uint32_t nowMs) {
  if (haveSeq_ && static_cast<uint32_t>(nowMs - lastGoodMs_) <= LINK_TIMEOUT_MS && !inGrace(nowMs)) {
    st_.lostFrames += static_cast<uint8_t>(seq - lastSeq_ - 1);
  }
  haveSeq_ = true;
  lastSeq_ = seq;
  lastGoodMs_ = nowMs;
  anySeen_ = true;
  st_.framesOk++;
  if (type == ML_TYPE_SIGNALS) {
    std::memcpy(&sig_, p, sizeof(sig_));
    sigSeen_ = true;
    sigMs_ = nowMs;
    if (sink_) sink_->onSignals(sig_);
  } else if (type == ML_TYPE_INFO) {
    std::memcpy(&st_.info, p, sizeof(MlInfo));
    st_.info.name[sizeof(st_.info.name) - 1] = 0;
    for (char* c = st_.info.name; *c; c++) {  // printable ASCII only (shown on the screen)
      if (*c < 0x20 || *c > 0x7E) *c = '?';
    }
    st_.infoValid = true;
    infoMs_ = nowMs;
    if (sink_) sink_->onInfo(st_.info);
  } else if (type == ML_TYPE_CAN) {
    MlCanHead h;
    std::memcpy(&h, p, sizeof(h));
    st_.canFrames++;
    if (sink_) {
      sink_->onCan(static_cast<uint8_t>((h.info >> 7) & 1), static_cast<uint8_t>((h.info >> 6) & 1),
                   h.id, static_cast<uint8_t>(h.info & 0x0F), p + sizeof(h), nowMs);
    }
  }
}

void Parser::feed(const uint8_t* data, size_t n, uint32_t nowMs) {
  rxBytes_ += static_cast<uint32_t>(n);
  while (n > 0) {
    const size_t room = sizeof(buf_) - static_cast<size_t>(len_);
    const size_t take = n < room ? n : room;
    std::memcpy(buf_ + len_, data, take);
    len_ += static_cast<int>(take);
    data += take;
    n -= take;

    int i = 0;
    for (;;) {
      while (i + 1 < len_ && !(buf_[i] == ML_SYNC0 && buf_[i + 1] == ML_SYNC1)) {
        i++;
        st_.junkBytes++;
      }
      if (len_ - i < 5) break;  // wait for type, seq and length
      const uint8_t type = buf_[i + 2], seq = buf_[i + 3], len = buf_[i + 4];
      if (len > ML_MAX_PAYLOAD) {
        i++;
        st_.junkBytes++;
        continue;
      }
      const int total = static_cast<int>(len) + ML_OVERHEAD;
      if (len_ - i < total) break;  // wait for the rest
      if (!plausible(type, len, buf_ + i + 5)) {
        i++;
        st_.junkBytes++;
        continue;
      }
      const uint16_t crc = mlCrc16(buf_ + i + 2, static_cast<size_t>(len) + 3);
      const uint16_t got = static_cast<uint16_t>(buf_[i + 5 + len] | (buf_[i + 6 + len] << 8));
      if (crc == got) {
        handle(type, seq, buf_ + i + 5, nowMs);
        i += total;
      } else {
        if (!inGrace(nowMs)) st_.crcErrors++;
        i++;  // drop only the first sync byte and look again (a good frame may follow inside)
      }
    }
    if (i > 0) {
      std::memmove(buf_, buf_ + i, static_cast<size_t>(len_ - i));
      len_ -= i;
    }
  }
}

void Parser::tick(uint32_t nowMs) {
  if (static_cast<uint32_t>(nowMs - tickMs_) >= 1000) {
    tickMs_ = nowMs;
    st_.framesPerSec = st_.framesOk - tickFrames_;
    tickFrames_ = st_.framesOk;
    st_.canPerSec = st_.canFrames - tickCan_;
    tickCan_ = st_.canFrames;
    st_.bytesPerSec = rxBytes_ - tickBytes_;
    tickBytes_ = rxBytes_;
    if (st_.infoValid && static_cast<uint32_t>(nowMs - infoMs_) > 3000) st_.infoValid = false;
    // Long gaps go back to "not seen" (linkUp/signalsUp compare signed differences, so an old stamp
    // must not stay forever)
    if (sigSeen_ && static_cast<uint32_t>(nowMs - sigMs_) > 2 * LINK_TIMEOUT_MS) sigSeen_ = false;
    if (anySeen_ && static_cast<uint32_t>(nowMs - lastGoodMs_) > 2 * LINK_TIMEOUT_MS) anySeen_ = false;
  }
}

void Parser::resyncSeq() {
  haveSeq_ = false;
  len_ = 0;
}

uint32_t Parser::autoBaud(uint32_t nowMs, size_t rxBytes) {
  if (!abStarted_) {
    abStarted_ = true;
    abWinMs_ = nowMs;
    abWinRx_ = 0;
    abWinGood_ = st_.framesOk;
    abBadWins_ = 0;
  }
  abWinRx_ += static_cast<uint32_t>(rxBytes);
  bool locked = haveSeq_ && static_cast<uint32_t>(nowMs - lastGoodMs_) <= LINK_TIMEOUT_MS;
  uint32_t next = 0;
  if (static_cast<uint32_t>(nowMs - abWinMs_) >= LINK_BAUD_WINDOW_MS) {
    // bytes arrived in this window but not a single good frame
    const bool bad = abWinRx_ >= LINK_BAUD_MIN_BYTES && st_.framesOk == abWinGood_;
    abBadWins_ = bad ? abBadWins_ + 1 : 0;
    abWinMs_ = nowMs;
    abWinRx_ = 0;
    abWinGood_ = st_.framesOk;
    if (abBadWins_ >= LINK_BAUD_BAD_WINDOWS) {
      abBadWins_ = 0;
      abIdx_ = (abIdx_ + 1) % kNumBauds;
      len_ = 0;  // bytes read at the old baud are dropped
      next = kBauds[abIdx_];
      locked = false;
    }
  }
  st_.baud = kBauds[abIdx_];
  st_.locked = locked;
  return next;
}

// Stamps come from the receive loop; a frame read after the caller took nowMs is newer (negative
// difference) and still counts as up.
bool Parser::signalsUp(uint32_t nowMs) const {
  return sigSeen_ && static_cast<int32_t>(nowMs - sigMs_) <= static_cast<int32_t>(LINK_TIMEOUT_MS);
}

bool Parser::linkUp(uint32_t nowMs) const {
  return anySeen_ && static_cast<int32_t>(nowMs - lastGoodMs_) <= static_cast<int32_t>(LINK_TIMEOUT_MS);
}

}  // namespace mlink
}  // namespace lt
