// gnss_uart.cpp - see gnss_uart.h
#include "gnss_uart.h"

#include <Arduino.h>
#include <string.h>

#include "../board.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"

namespace hw {

namespace {

constexpr uart_port_t kPort = UART_NUM_1;
constexpr int kNumBauds = static_cast<int>(sizeof(GNSS_BAUDS) / sizeof(GNSS_BAUDS[0]));
constexpr uint32_t kSwitchWaitMs = 100;  // after the baud command left the UART (round gauge gpsRetry)
constexpr uint32_t kBlindGapMs = 20;     // between the baud commands of the blind pass
constexpr uint32_t kFoundFrames = 2;     // good UBX frames + NMEA sentences that identify the baud

bool s_uartOk = false;
GnssState s_state = GnssState::Off;
uint32_t s_stateMs = 0;
uint32_t s_baud = 0;
uint32_t s_foundBaud = 0;
uint32_t s_attempts = 0;
int s_probeIdx = 0;
int s_blindIdx = 0;
bool s_txDone = false;
uint32_t s_txDoneMs = 0;
uint32_t s_seen0 = 0, s_ack0 = 0, s_nak0 = 0, s_pvt0 = 0, s_monVer0 = 0;
bool s_configAck = false, s_configNak = false;
bool s_periodSet = false;
uint32_t s_lastPvtMs = 0;
uint32_t s_lastPvtCount = 0;
lt::ubx::Parser s_ubx;  // own parser for the start-up and the status (the caller parses for itself)
uint8_t s_scratch[512];
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
GnssStatus s_pub = {};  // published copy (gnssStatus)

uint32_t seen() { return s_ubx.counters().frames + s_ubx.counters().nmea; }

bool logNow() { return s_attempts <= 4 || s_attempts % 30 == 0; }  // like the round gauge's retry log

void setBaud(uint32_t b) {
  uart_set_baudrate(kPort, b);
  uart_flush_input(kPort);
  s_baud = b;
}

void send(const uint8_t* d, size_t n) {
  if (n) uart_write_bytes(kPort, d, n);
}

void enter(GnssState st, uint32_t now) {
  s_state = st;
  s_stateMs = now;
}

void startProbe(int idx, uint32_t now) {
  s_configAck = s_configNak = false;
  s_probeIdx = idx;
  setBaud(GNSS_BAUDS[idx]);
  s_seen0 = seen();
  enter(GnssState::Probe, now);
}

void sendConfig(uint32_t now) {
  uint8_t b[64];
  send(b, lt::ubx::buildNavConfig(b, sizeof(b), GNSS_RATE_HZ));
  send(b, lt::ubx::buildPollMonVer(b, sizeof(b)));
  s_ack0 = s_ubx.counters().ackValset;
  s_nak0 = s_ubx.counters().nakValset;
  s_configAck = s_configNak = false;
  enter(GnssState::Config, now);
}

void sendBaudCommand() {
  uint8_t b[32];
  send(b, lt::ubx::buildBaud(b, sizeof(b), GNSS_BAUD));
  s_txDone = false;
}

// true once the queued bytes have left the UART; then waits `ms`
bool txSettled(uint32_t now, uint32_t ms) {
  if (!s_txDone) {
    if (uart_wait_tx_done(kPort, 0) != ESP_OK) return false;
    s_txDone = true;
    s_txDoneMs = now;
  }
  return static_cast<uint32_t>(now - s_txDoneMs) >= ms;
}

void startBlind(uint32_t now) {
  s_blindIdx = 1;  // every baud but the target
  setBaud(GNSS_BAUDS[s_blindIdx]);
  sendBaudCommand();
  enter(GnssState::Blind, now);
}

void step(uint32_t now) {
  const lt::ubx::Counters& c = s_ubx.counters();
  switch (s_state) {
    case GnssState::Probe:
      if (seen() - s_seen0 >= kFoundFrames) {
        s_foundBaud = s_baud;
        Serial.printf("GNSS: receiver heard at %lu bps (UBX %lu, NMEA %lu)\n", static_cast<unsigned long>(s_baud),
                      static_cast<unsigned long>(c.frames), static_cast<unsigned long>(c.nmea));
        if (s_baud == GNSS_BAUD) {
          sendConfig(now);
        } else {
          sendBaudCommand();  // at the receiver's current baud
          enter(GnssState::Switch, now);
        }
      } else if (static_cast<uint32_t>(now - s_stateMs) >= GNSS_PROBE_MS) {
        if (s_probeIdx + 1 < kNumBauds) {
          startProbe(s_probeIdx + 1, now);
        } else {
          s_attempts++;
          if (logNow()) {
            Serial.printf("GNSS: nothing heard at any baud (%lu). Sending the %lu bps command at every baud (RX GPIO%d, TX GPIO%d)\n",
                          static_cast<unsigned long>(s_attempts), static_cast<unsigned long>(GNSS_BAUD),
                          board::GNSS_RX, board::GNSS_TX);
          }
          startBlind(now);
        }
      }
      break;
    case GnssState::Switch:
      if (txSettled(now, kSwitchWaitMs)) {
        setBaud(GNSS_BAUD);
        sendConfig(now);
      }
      break;
    case GnssState::Blind:
      if (txSettled(now, kBlindGapMs)) {
        if (++s_blindIdx < kNumBauds) {
          setBaud(GNSS_BAUDS[s_blindIdx]);
          sendBaudCommand();
        } else {
          setBaud(GNSS_BAUD);
          sendConfig(now);
        }
      }
      break;
    case GnssState::Config:
      if (c.ackValset != s_ack0 || c.nakValset != s_nak0 ||
          static_cast<uint32_t>(now - s_stateMs) >= GNSS_ACK_MS) {
        s_configAck = c.ackValset != s_ack0;
        s_configNak = c.nakValset != s_nak0;
        if (s_configNak || (!s_configAck && logNow())) {
          Serial.printf("GNSS: nav config (CFG-VALSET) %s\n", s_configNak ? "rejected (NAK)" : "not acknowledged");
        }
        s_pvt0 = c.pvt;
        enter(GnssState::Verify, now);
      }
      break;
    case GnssState::Verify:
      if (c.pvt != s_pvt0) {
        if (!s_periodSet) {
          s_ubx.setEpochPeriodMs(1000 / GNSS_RATE_HZ);
          s_periodSet = true;
        }
        s_lastPvtMs = now;
        s_attempts = 0;
        Serial.printf("GNSS: NAV-PVT arriving (%lu bps, config ACK %s, RX GPIO%d, TX GPIO%d)\n",
                      static_cast<unsigned long>(s_baud), s_configAck ? "yes" : "no", board::GNSS_RX,
                      board::GNSS_TX);
        enter(GnssState::Run, now);
      } else if (static_cast<uint32_t>(now - s_stateMs) >= RC_GPS_RETRY_MS) {
        s_attempts++;
        if (logNow()) {
          Serial.printf("GNSS: no NAV-PVT (%lu). Probing the baud again\n", static_cast<unsigned long>(s_attempts));
        }
        startProbe(0, now);
      }
      break;
    case GnssState::Run:
      if (static_cast<uint32_t>(now - s_lastPvtMs) > RC_GPS_LOST_MS) {
        Serial.println("GNSS: NAV-PVT stopped. Sending the config again");
        setBaud(GNSS_BAUD);
        sendConfig(now);
      }
      break;
    case GnssState::Off:
    default:
      break;
  }
}

void publish() {
  const lt::ubx::Counters& c = s_ubx.counters();
  GnssStatus st = {};
  st.uartOk = s_uartOk;
  st.state = s_state;
  st.baud = s_baud;
  st.foundBaud = s_foundBaud;
  st.attempts = s_attempts;
  st.configured = s_configAck;
  st.configNak = s_configNak;
  st.running = s_state == GnssState::Run;
  st.pvtPerSec = c.pvtPerSec;
  const int d = static_cast<int>(c.pvtPerSec) - GNSS_RATE_HZ;
  st.rateOk = st.running && d >= -1 && d <= 1;
  st.lastPvtMs = s_lastPvtMs;
  st.ubx = c;
  const bool newVer = c.monVer != s_monVer0;
  if (newVer) {
    s_monVer0 = c.monVer;
    s_ubx.monVer(st.swVersion, sizeof(st.swVersion), st.hwVersion, sizeof(st.hwVersion));
    if (!s_ubx.monVerFind("MOD=", st.module, sizeof(st.module))) st.module[0] = 0;
    Serial.printf("GNSS: receiver %s / %s / %s\n", st.module[0] ? st.module : "?", st.swVersion, st.hwVersion);
  }
  taskENTER_CRITICAL(&s_mux);
  if (!newVer) {
    memcpy(st.swVersion, s_pub.swVersion, sizeof(st.swVersion));
    memcpy(st.hwVersion, s_pub.hwVersion, sizeof(st.hwVersion));
    memcpy(st.module, s_pub.module, sizeof(st.module));
  }
  s_pub = st;
  taskEXIT_CRITICAL(&s_mux);
}

}  // namespace

const char* gnssStateName(GnssState s) {
  switch (s) {
    case GnssState::Off: return "off";
    case GnssState::Probe: return "probe";
    case GnssState::Switch: return "switch";
    case GnssState::Config: return "config";
    case GnssState::Verify: return "verify";
    case GnssState::Run: return "run";
    case GnssState::Blind: return "blind";
  }
  return "?";
}

bool gnssBegin() {
  if (s_uartOk) return true;
  uart_config_t cfg = {};
  cfg.baud_rate = static_cast<int>(GNSS_BAUD);
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;
  // buffer sizes of the round gauge (RX 4096 = 1.6 s of 25 Hz NAV-PVT, TX 512)
  if (uart_driver_install(kPort, 4096, 512, 0, nullptr, 0) != ESP_OK) return false;
  if (uart_param_config(kPort, &cfg) != ESP_OK) return false;
  gpio_pullup_en(static_cast<gpio_num_t>(board::GNSS_RX));  // idle-high when the cable is off
  if (uart_set_pin(kPort, board::GNSS_TX, board::GNSS_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
    return false;
  }
  s_uartOk = true;
  s_ubx.begin();
  s_ubx.setEpochPeriodMs(0);
  s_periodSet = false;
  const uint32_t now = millis();
  s_ubx.tick(now);
  startProbe(0, now);
  publish();
  return true;
}

size_t gnssPoll(uint8_t* buf, size_t cap) {
  if (!s_uartOk) return 0;
  const uint32_t now = millis();
  const bool give = (s_state == GnssState::Config || s_state == GnssState::Verify || s_state == GnssState::Run) &&
                    buf && cap;
  uint8_t* dst = give ? buf : s_scratch;
  const size_t room = give ? cap : sizeof(s_scratch);
  size_t avail = 0;
  uart_get_buffered_data_len(kPort, &avail);
  if (avail > room) avail = room;
  int n = avail ? uart_read_bytes(kPort, dst, avail, 0) : 0;
  if (n < 0) n = 0;
  if (n > 0) s_ubx.feed(dst, static_cast<size_t>(n), now);
  if (s_ubx.counters().pvt != s_lastPvtCount) {
    s_lastPvtCount = s_ubx.counters().pvt;
    if (s_state == GnssState::Run) s_lastPvtMs = now;
  }
  s_ubx.tick(now);
  step(now);
  publish();
  return give ? static_cast<size_t>(n) : 0;
}

void gnssStatus(GnssStatus* out) {
  taskENTER_CRITICAL(&s_mux);
  *out = s_pub;
  taskEXIT_CRITICAL(&s_mux);
}

}  // namespace hw
