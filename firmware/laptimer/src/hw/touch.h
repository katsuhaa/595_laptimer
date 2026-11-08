// touch.h - FT6336 touch controller (FT5x06 register set, I2C 0x38) and the board's shared I2C bus.
//
// I2C bus: Wire on board::I2C_SDA / board::I2C_SCL at 300 kHz (every Waveshare 2.41 demo: i2c_bsp clk_speed 300 kHz).
// Devices: touch 0x38, IMU 0x6B, RTC 0x51, IO expander 0x20. Only the hw drivers use Wire, always through these
// helpers. A recursive mutex makes each transfer (and multi-transfer sequences such as the IMU reset) atomic, so the
// drivers may be called from different tasks.
//
// Touch: polled (TP_INT is on the IO expander and not used). Read procedure and coordinate transform are those of
// the demo (esp_lcd_touch_ft5x06.c + esp_lcd_touch.c with the 09_LVGL_Test Rotate_90 config: x_max 449, y_max 599,
// swap_xy 1, mirror_x 0, mirror_y 1), giving coordinates in the 600x450 landscape frame of display.h.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace hw {

// ---- shared I2C bus
bool i2cBegin();  // idempotent; call once from setup() before the tasks start
bool i2cLock(uint32_t timeoutMs = 1000);
void i2cUnlock();
bool i2cWriteReg(uint8_t addr, uint8_t reg, const uint8_t* data, size_t n);  // [reg][data...] with stop
bool i2cReadReg(uint8_t addr, uint8_t reg, uint8_t* buf, size_t n);         // [reg], repeated start, n bytes
bool i2cProbe(uint8_t addr);                                                // address ACK

// ---- touch
struct TouchStats {
  bool ok;           // controller answered (touchBegin, or any later read)
  uint32_t readyMs;  // time from the end of the reset until the first ACK in touchBegin()
  uint32_t reads;    // touchRead() calls
  uint32_t errors;   // touchRead() calls with an I2C error
};

bool touchBegin();                // reset on board::TP_RST, then waits for the controller to answer (<= 0.5 s)
bool touchRead(int* x, int* y);   // true while pressed; x 0..599, y 0..449 (unchanged when not pressed)
void touchStats(TouchStats* out);

}  // namespace hw
