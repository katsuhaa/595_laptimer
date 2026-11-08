// link_uart.cpp - see link_uart.h
#include "link_uart.h"

#include <Arduino.h>

#include "../board.h"
#include "driver/gpio.h"
#include "driver/uart.h"

namespace hw {

namespace {

constexpr uart_port_t kPort = UART_NUM_2;
constexpr int kRxBuffer = 16384;  // round gauge linkBegin
constexpr int kMaxChunks = 16;    // 16 x 1024 bytes per call at most

bool s_ok = false;
uint32_t s_baud = 0;
uint8_t s_buf[1024];  // round gauge linkTask buffer

void setBaud(uint32_t b) {
  uart_set_baudrate(kPort, b);
  uart_flush_input(kPort);  // bytes of the old baud are junk
  s_baud = b;
}

}  // namespace

bool linkBegin() {
  if (s_ok) return true;
  uart_config_t cfg = {};
  cfg.baud_rate = static_cast<int>(lt::mlink::Parser::candidate(0));
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;
  if (uart_driver_install(kPort, kRxBuffer, 0, 0, nullptr, 0) != ESP_OK) return false;
  if (uart_param_config(kPort, &cfg) != ESP_OK) return false;
  if (uart_set_pin(kPort, UART_PIN_NO_CHANGE, board::LINK_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
    return false;
  }
  gpio_pullup_en(static_cast<gpio_num_t>(board::LINK_RX));  // idle-high when the cable is off
  s_baud = lt::mlink::Parser::candidate(0);
  s_ok = true;
  return true;
}

size_t linkPoll(lt::mlink::Parser& p, uint32_t nowMs) {
  if (!s_ok) return 0;
  if (p.baud() != s_baud) setBaud(p.baud());  // e.g. after p.begin()
  size_t total = 0;
  for (int k = 0; k < kMaxChunks; k++) {
    size_t avail = 0;
    uart_get_buffered_data_len(kPort, &avail);
    if (!avail) break;
    if (avail > sizeof(s_buf)) avail = sizeof(s_buf);
    const int n = uart_read_bytes(kPort, s_buf, avail, 0);
    if (n <= 0) break;
    p.feed(s_buf, static_cast<size_t>(n), nowMs);
    total += static_cast<size_t>(n);
  }
  p.tick(nowMs);
  const uint32_t next = p.autoBaud(nowMs, total);  // bytes but no good frame for 1.5 s: next candidate
  if (next) {
    setBaud(next);
    Serial.printf("LINK: no good frame, switched to %lu bps\n", static_cast<unsigned long>(next));
  }
  return total;
}

uint32_t linkBaud() { return s_baud; }

}  // namespace hw
