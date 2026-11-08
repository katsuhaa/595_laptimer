// link_uart.h - receive-only UART2 on board::LINK_RX for the round gauge's CAN reader box (XIAO ESP32C6 running
// c6_can_link 2.x: raw CAN frames of C-CAN and B-CAN as meter_link v2 at 921600 bps).
// Same as the round gauge's linkBegin()/linkTask() (meter_display.ino): 16 KB RX buffer, pull-up on the RX pin
// (idle when the cable is off), auto-baud by lt::mlink::Parser::autoBaud() (METER_LINK_BAUD_CANDIDATES), the UART
// follows the parser's baud and is flushed on every change.
// linkPoll() does not wait: when it returns 0 the caller sleeps a little (the round gauge waited up to 5 ms for one
// byte; at 921600 bps the 16 KB buffer holds about 170 ms). Call it from one task only.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "lt_core.h"

namespace hw {

bool linkBegin();  // UART2 at the first candidate baud (921600); false = UART error
// Reads what is buffered (up to 16 KB per call), feeds p, runs p.tick() and p.autoBaud().
// Returns the bytes read.
size_t linkPoll(lt::mlink::Parser& p, uint32_t nowMs);
uint32_t linkBaud();  // UART baud now (0 before linkBegin)

}  // namespace hw
