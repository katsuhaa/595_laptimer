// core/mlink.h - receiver of the meter_link v2 stream from the CAN reader box (XIAO ESP32C6 running
// c6_can_link 2.x). Port of the round gauge's `mlink` (katsuhaa/abarth595-round-gauge 05ab0a1,
// meter_display/app.cpp `mlink`, config.h LINK_*), same values and steps, without globals and locks:
//  - frames 0xA5 0x5A | type | seq | len | payload | CRC-16/CCITT-FALSE (LE), see meter_link.h
//  - a frame is used only when its type and length fit (plausible) and the CRC matches; on a CRC
//    error only the first sync byte is dropped, so a good frame inside the bad one is still found
//  - lost frames from sequence gaps, but not after a pause longer than LINK_TIMEOUT_MS and not
//    during a grace period (flash writes delay the UART); CRC errors are not counted in grace either
//  - 1-second rates (tick), receiver state (MlInfo) valid for 3 s
//  - auto-baud: when bytes arrive (>= LINK_BAUD_MIN_BYTES in a LINK_BAUD_WINDOW_MS window) but no good
//    frame for LINK_BAUD_BAD_WINDOWS windows in a row, move to the next METER_LINK_BAUD_CANDIDATES entry
// Output goes to a Sink. Not thread-safe: feed/tick/autoBaud from one task, or guard the calls.
#pragma once

#include <cstddef>
#include <cstdint>

#include "meter_link.h"

namespace lt {
namespace mlink {

// config.h of the round gauge
constexpr uint32_t LINK_TIMEOUT_MS = 1000;     // no frame for this long = no signal
constexpr uint32_t LINK_BAUD_WINDOW_MS = 500;  // auto-baud window
constexpr int LINK_BAUD_BAD_WINDOWS = 3;       // bad windows in a row before the next baud (1.5 s)
constexpr uint32_t LINK_BAUD_MIN_BYTES = 16;   // fewer bytes in a window = nothing arrives (stay)

// Receives the frames. Override what you need; every call comes from feed().
class Sink {
 public:
  // One raw CAN frame. bus 0 = C-CAN, 1 = B-CAN; ext 1 = 29-bit id; dlc 0..8
  virtual void onCan(uint8_t /*bus*/, uint8_t /*ext*/, uint32_t /*id*/, uint8_t /*dlc*/,
                     const uint8_t* /*data*/, uint32_t /*nowMs*/) {}
  // Sender state (1/s). name is NUL-terminated and printable ASCII.
  virtual void onInfo(const MlInfo& /*info*/) {}
  // Value frame (c6_display_debug, 1.x c6_can_link; c6_can_link 2.x does not send it)
  virtual void onSignals(const MlSignals& /*s*/) {}

 protected:
  ~Sink() = default;
};

// LinkStats of the round gauge, plus CAN counts
struct Stats {
  uint32_t baud;          // baud the receiver should use now
  bool locked;            // good frames at this baud within LINK_TIMEOUT_MS (updated by autoBaud)
  uint32_t framesOk;      // good frames (all types)
  uint32_t crcErrors;     // CRC mismatches (not counted during grace)
  uint32_t lostFrames;    // frames missing from sequence gaps
  uint32_t junkBytes;     // bytes dropped while looking for frames (boot log of the XIAO etc.)
  uint32_t framesPerSec;  // good frames in the last second (tick)
  uint32_t bytesPerSec;   // bytes in the last second, frames or not (tick)
  bool infoValid;         // MlInfo received within 3 s
  MlInfo info;            // last MlInfo
  uint32_t canFrames;     // good CAN frames (total)
  uint32_t canPerSec;     // good CAN frames in the last second (tick)
};

class Parser {
 public:
  explicit Parser(Sink* sink = nullptr) : sink_(sink) { begin(); }
  void setSink(Sink* sink) { sink_ = sink; }

  void begin();  // forget everything; the baud goes back to the first candidate
  void feed(const uint8_t* data, size_t n, uint32_t nowMs);
  void tick(uint32_t nowMs);  // 1-second statistics; call often (every receive loop)
  // Call once per receive loop with the bytes read in that loop. Returns the new baud when the
  // receiver must switch (the caller changes the UART and flushes its input), else 0.
  uint32_t autoBaud(uint32_t nowMs, size_t rxBytes);
  uint32_t baud() const { return candidate(abIdx_); }
  // CRC errors and lost frames found before untilMs are not counted (flash writes)
  void graceUntil(uint32_t untilMs);
  // The source changed (e.g. a demo feed): the next frame's sequence gap is not counted
  void resyncSeq();

  bool linkUp(uint32_t nowMs) const;     // any good frame within LINK_TIMEOUT_MS
  bool signalsUp(uint32_t nowMs) const;  // a value frame within LINK_TIMEOUT_MS
  // Values come from the raw CAN frames now (no value frame, but the link is up)
  bool usingRaw(uint32_t nowMs) const { return !signalsUp(nowMs) && linkUp(nowMs); }
  const MlSignals& lastSignals() const { return sig_; }
  const Stats& stats() const { return st_; }
  void getStats(Stats* out) const { *out = st_; }  // round gauge mlink::getStats

  static int candidates();          // number of METER_LINK_BAUD_CANDIDATES
  static uint32_t candidate(int i);  // METER_LINK_BAUD_CANDIDATES[i]

 private:
  static bool plausible(uint8_t type, uint8_t len, const uint8_t* payload);
  bool inGrace(uint32_t nowMs);
  void handle(uint8_t type, uint8_t seq, const uint8_t* p, uint32_t nowMs);

  Sink* sink_ = nullptr;
  uint8_t buf_[192];
  int len_ = 0;

  MlSignals sig_{};
  bool sigSeen_ = false;
  uint32_t sigMs_ = 0;

  Stats st_{};
  uint32_t infoMs_ = 0;
  bool haveSeq_ = false;
  uint8_t lastSeq_ = 0;
  uint32_t lastGoodMs_ = 0;
  bool anySeen_ = false;
  uint32_t tickMs_ = 0, tickFrames_ = 0, tickCan_ = 0;
  uint32_t rxBytes_ = 0, tickBytes_ = 0;
  bool graceOn_ = false;
  uint32_t graceUntil_ = 0;

  int abIdx_ = 0;
  bool abStarted_ = false;
  uint32_t abWinMs_ = 0, abWinRx_ = 0, abWinGood_ = 0;
  int abBadWins_ = 0;
};

}  // namespace mlink
}  // namespace lt
