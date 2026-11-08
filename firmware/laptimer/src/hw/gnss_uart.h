// gnss_uart.h - Matek M9N-5883 (u-blox NEO-M9N) on UART1, pins board::GNSS_RX (<- M9N TX) and board::GNSS_TX
// (-> M9N RX). Plain 8N1 UART like the round gauge 2.14.3+ (it dropped its RX-pin detection and open-drain TX at
// the owner's request, so neither is ported).
//
// Start-up, run by gnssPoll() without blocking (state names in GnssState):
//   Probe   listen at each GNSS_BAUDS entry (230400 = target first, then 38400 = M9N factory, 115200, 9600) for
//           GNSS_PROBE_MS; found = 2 good UBX frames or NMEA sentences
//   Switch  found at another baud: CFG-VALSET UART1 baud = GNSS_BAUD sent there, then the UART follows (100 ms,
//           like the round gauge's gpsRetry)
//   Config  at GNSS_BAUD: CFG-VALSET nav config (25 Hz, NAV-PVT only, NMEA off, automotive; RAM layer) and a
//           MON-VER poll; waits GNSS_ACK_MS for the VALSET ACK/NAK
//   Verify  waits RC_GPS_RETRY_MS for NAV-PVT, else probes again
//   Run     NAV-PVT flowing; the rate of the last second is checked against GNSS_RATE_HZ. No NAV-PVT for
//           RC_GPS_LOST_MS: config sent again (round gauge gpsManage), then probing if that does not help
//   Blind   nothing heard at any baud: the baud command is sent at every other baud without listening (the round
//           gauge's gpsRetry), then Config at GNSS_BAUD
// gnssPoll() returns the received bytes (for the caller's lt::ubx::Parser and the raw .ubx log) once the baud is
// set (Config/Verify/Run); bytes read while probing other bauds are dropped. Call it from one task every few ms.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "lt_core.h"

namespace hw {

constexpr uint32_t GNSS_BAUD = 230400;  // UART1 baud in use (プロジェクト計画 3.3: 230400 or more)
constexpr int GNSS_RATE_HZ = 25;        // navigation rate (NEO-M9N maximum)
inline constexpr uint32_t GNSS_BAUDS[4] = {230400, 38400, 115200, 9600};  // probe order
constexpr uint32_t GNSS_PROBE_MS = 1200;  // per baud: the factory NMEA comes once per second
constexpr uint32_t GNSS_ACK_MS = 1000;    // wait for the CFG-VALSET ACK
constexpr uint32_t RC_GPS_RETRY_MS = 2000;  // round gauge: no NAV-PVT this long after the config -> again
constexpr uint32_t RC_GPS_LOST_MS = 3000;   // round gauge: NAV-PVT stopped this long -> config again

enum class GnssState : uint8_t { Off, Probe, Switch, Config, Verify, Run, Blind };
const char* gnssStateName(GnssState s);  // "off", "probe", ...

struct GnssStatus {
  bool uartOk;            // UART1 driver installed
  GnssState state;
  uint32_t baud;          // UART baud now
  uint32_t foundBaud;     // baud the receiver was heard at (0 = not yet)
  uint32_t attempts;      // full probe rounds without success
  bool configured;        // the last nav config was acknowledged at GNSS_BAUD (ACK-ACK for CFG-VALSET)
  bool configNak;         // ... or rejected (ACK-NAK)
  bool running;           // NAV-PVT arrives (state Run)
  bool rateOk;            // NAV-PVT of the last second within GNSS_RATE_HZ +- 1
  uint32_t pvtPerSec;     // NAV-PVT in the last full second
  uint32_t lastPvtMs;     // millis of the last NAV-PVT (0 = none)
  lt::ubx::Counters ubx;  // counters of the internal parser (checksum errors, NMEA, missed epochs, ...)
  char swVersion[31];     // MON-VER (empty until answered)
  char hwVersion[11];
  char module[31];        // MON-VER extension "MOD=" (e.g. "NEO-M9N")
};

bool gnssBegin();                            // installs UART1 and starts probing; false = UART error
size_t gnssPoll(uint8_t* buf, size_t cap);  // reads, runs the start-up, returns bytes copied into buf
void gnssStatus(GnssStatus* out);           // copy (any task)

}  // namespace hw
