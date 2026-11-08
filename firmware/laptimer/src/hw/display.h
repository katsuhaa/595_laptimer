// display.h - RM690B0 AMOLED of the Waveshare ESP32-S3-Touch-AMOLED-2.41, used as a 600x450 landscape screen.
//
// Transport: QSPI on SPI2_HOST with the ESP-IDF spi_master driver, written like the round gauge's own driver
// (meter_display.ino lcdCmd / lcdSetWindow / lcdSendChunk / lcdPush): CS is driven by hand so that one window of
// pixels goes out as several DMA transactions while CS stays low. Two internal DMA bounce buffers: one is filled
// (byte swap from the PSRAM framebuffer) while the other is on the wire.
// Panel side (commands, init list, orientation, window rules) is the Waveshare 2.41 demo
// (Arduino/examples/09_LVGL_Test: lcd_init_cmds[], esp_lcd_sh8601.c, example_lvgl_flush_cb, example_lvgl_rounder_cb).
//
// All functions may be called from any task (one mutex serializes the SPI traffic). displayBrightness() waits for a
// push that is in progress (at most one full frame, ~30-40 ms).
#pragma once
#include <stdint.h>

namespace hw {

struct DisplayStats {
  bool ok;                // displayBegin() succeeded
  int32_t spiError;       // first error code (esp_err_t) returned by the SPI driver; 0 = none
  uint32_t spiKHz;        // QSPI clock really used (spi_device_get_actual_freq); 40000 requested
  uint32_t pushes;        // completed displayPush() / displayFill() calls
  uint32_t lastPushUs;    // duration of the last push [us] (window commands + all pixel data)
  uint32_t maxPushUs;     // longest push since the last displayStats(..., true)
  uint32_t lastFullUs;    // duration of the last full-screen (600x450) push [us]; 0 = none yet
  uint64_t bytes;         // pixel bytes sent (after the even/odd rounding of the rectangles)
  uint8_t brightnessPct;  // brightness last sent to the panel; 0 until the first frame was sent
};

// Bus, reset, init list, then clears the panel memory to black. The panel is on, but its brightness stays 0 until the
// first displayPush()/displayFill() has completed; then the value of displayBrightness() (default 100 %) is sent.
bool displayBegin();

// Sends the rectangle (x, y, w, h) of fb to the panel. fb = the whole 600x450 RGB565 frame (stride 600, native-endian
// uint16 as the round gauge renderer writes it; it may live in PSRAM). The rectangle is clipped to the screen and grown
// to the panel's rule (start rounded down to even, end up to odd; demo example_lvgl_rounder_cb), so up to one extra
// row/column of fb around it is sent too. Returns false if the display is not ready or the SPI driver reported an error.
bool displayPush(const uint16_t* fb, int x, int y, int w, int h);

// Fills the whole panel with one color without a framebuffer (e.g. "red = no PSRAM", as the round gauge lcdFillSolid).
bool displayFill(uint16_t rgb565);

// 0-100 %, sent as 0x51 = pct * 255 / 100 (round gauge lcdBrightness; demo setBrightnes writes 0x51 the same way).
void displayBrightness(uint8_t pct);

void displayStats(DisplayStats* out, bool resetMax = false);

}  // namespace hw
