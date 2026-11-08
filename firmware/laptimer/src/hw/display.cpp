// display.cpp - see display.h
#include "display.h"

#include <Arduino.h>
#include <string.h>

#include "../board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace hw {
namespace {

constexpr int W = board::LCD_W;  // 600
constexpr int H = board::LCD_H;  // 450

constexpr int kSpiHz = 40 * 1000 * 1000;  // demo SH8601_PANEL_IO_QSPI_CONFIG pclk_hz; round gauge LCD_SPI_HZ
constexpr size_t kChunk = 16384;          // bytes per DMA transaction (round gauge LCD_CHUNK_BYTES)
constexpr size_t kChunkPx = kChunk / 2;

// QSPI framing of esp_lcd_sh8601.c (vendor flag use_qspi_interface = 1, panel IO lcd_cmd_bits = 32):
//   tx_param(): (0x02 << 24) | (cmd << 8) on one data line, then the parameters on one line
//   tx_color(): (0x32 << 24) | (0x2C << 8) on one data line, then the pixel bytes on four lines, CS low throughout
// Sent here as the round gauge does it: 8-bit command phase + 24-bit address phase (+ data phase).
constexpr uint8_t kOpWriteCmd = 0x02;    // LCD_OPCODE_WRITE_CMD
constexpr uint8_t kOpWriteColor = 0x32;  // LCD_OPCODE_WRITE_COLOR
constexpr uint8_t kRamwr = 0x2C;         // LCD_CMD_RAMWR (the demo writes every window with RAMWR)

struct InitCmd {
  uint8_t cmd;
  uint8_t len;  // parameter bytes
  uint8_t data[4];
  uint8_t delayMs;
};

const InitCmd kInit[] = {
    // panel_sh8601_init() sends MADCTL and COLMOD from the panel config before the vendor list
    // (rgb_ele_order RGB -> madctl 0x00; bits_per_pixel 16 -> colmod 0x55)
    {0x36, 1, {0x00}, 0},
    {0x3A, 1, {0x55}, 0},
    // lcd_init_cmds[] of 09_LVGL_Test.ino with AMOLED_Rotate == Rotate_90: same order, lengths and delays
    {0xFE, 1, {0x20}, 0},
    {0x26, 1, {0x0A}, 0},
    {0x24, 1, {0x80}, 0},
    {0xFE, 1, {0x00}, 0},
    {0x3A, 1, {0x55}, 0},
    {0xC2, 1, {0x00}, 10},
    {0x35, 0, {0x00}, 0},  // data_bytes 0 in the demo: command only
    {0x51, 1, {0x00}, 10},
    {0x11, 0, {0x00}, 80},
    {0x2A, 4, {0x00, 0x10, 0x01, 0xD1}, 0},
    {0x2B, 4, {0x00, 0x00, 0x02, 0x57}, 0},
    {0x29, 0, {0x00}, 10},
    {0x36, 1, {0x30}, 0},  // rotated 600x450 (only with Rotate_90)
    // The demo list ends with 0x51 = 0xFF. Here the brightness stays 0 until the first frame was sent
    // (round gauge g_firstFrame); displayPush()/displayFill() then send the value of displayBrightness().
    // esp_lcd_panel_disp_on_off(panel, true), which the demo calls after esp_lcd_panel_init():
    {0x29, 0, {0x00}, 0},
};

spi_device_handle_t g_spi = nullptr;
uint8_t* g_buf[2] = {nullptr, nullptr};  // internal DMA memory, kChunk bytes each
spi_transaction_ext_t g_tr[2];           // one descriptor per buffer (both may be queued)
spi_transaction_ext_t g_cmdTr;
SemaphoreHandle_t g_mu = nullptr;
portMUX_TYPE g_statLock = portMUX_INITIALIZER_UNLOCKED;
DisplayStats g_stats = {};
bool g_ok = false;
bool g_firstDone = false;  // first frame sent (brightness may come up)
uint8_t g_wantPct = 100;   // brightness to use (sent once the first frame is out)

inline void cs(int level) {
  gpio_set_level((gpio_num_t)board::LCD_CS, level);
}

void noteErr(esp_err_t e) {
  if (e == ESP_OK) return;
  portENTER_CRITICAL(&g_statLock);
  if (g_stats.spiError == 0) g_stats.spiError = (int32_t)e;
  portEXIT_CRITICAL(&g_statLock);
}

// round gauge lcdCmd(): command 0x02, address = cmd << 8, up to 4 parameter bytes, one data line
void lcdCmd(uint8_t cmd, const uint8_t* data, int len) {
  memset(&g_cmdTr, 0, sizeof g_cmdTr);
  g_cmdTr.base.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR | (len ? SPI_TRANS_USE_TXDATA : 0);
  g_cmdTr.base.cmd = kOpWriteCmd;
  g_cmdTr.base.addr = (uint32_t)cmd << 8;
  for (int i = 0; i < len && i < 4; i++) g_cmdTr.base.tx_data[i] = data[i];
  g_cmdTr.base.length = (size_t)len * 8;
  cs(0);
  noteErr(spi_device_polling_transmit(g_spi, &g_cmdTr.base));
  cs(1);
}

void sendBrightness(uint8_t pct) {
  const uint8_t v = (uint8_t)(pct * 255 / 100);
  lcdCmd(0x51, &v, 1);
  portENTER_CRITICAL(&g_statLock);
  g_stats.brightnessPct = pct;
  portEXIT_CRITICAL(&g_statLock);
}

// Window in panel coordinates: columns = x, rows = y + 16 (demo example_lvgl_flush_cb with Rotate_90 and
// panel_sh8601_draw_bitmap: CASET x1..x2, RASET y1..y2, ends inclusive)
void setWindow(int x1, int y1, int x2, int y2) {
  const int r1 = y1 + board::LCD_ROW_OFFSET, r2 = y2 + board::LCD_ROW_OFFSET;
  const uint8_t ca[4] = {(uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2};
  const uint8_t ra[4] = {(uint8_t)(r1 >> 8), (uint8_t)r1, (uint8_t)(r2 >> 8), (uint8_t)r2};
  lcdCmd(0x2A, ca, 4);
  lcdCmd(0x2B, ra, 4);
}

// Queues g_buf[i] (bytes) as pixel data. first = start of the window: 0x32 + RAMWR address, later chunks continue
// with data only while CS stays low (round gauge lcdSendChunk).
bool queueChunk(int i, size_t bytes, bool first) {
  spi_transaction_ext_t& t = g_tr[i];
  memset(&t, 0, sizeof t);
  if (first) {
    t.base.flags = SPI_TRANS_MODE_QIO;
    t.base.cmd = kOpWriteColor;
    t.base.addr = (uint32_t)kRamwr << 8;
  } else {
    t.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
  }
  t.base.tx_buffer = g_buf[i];
  t.base.length = bytes * 8;
  const esp_err_t e = spi_device_queue_trans(g_spi, &t.base, portMAX_DELAY);
  noteErr(e);
  return e == ESP_OK;
}

bool waitOne() {
  spi_transaction_t* done = nullptr;
  const esp_err_t e = spi_device_get_trans_result(g_spi, &done, portMAX_DELAY);
  noteErr(e);
  return e == ESP_OK;
}

typedef uint32_t __attribute__((may_alias)) u32a;

// Native-endian RGB565 -> high byte first on the wire (round gauge lcdPush: __builtin_bswap16; the demo gets the same
// byte order from LV_COLOR_16_SWAP 1). n is even; dst is 4-byte aligned.
inline void swapCopy(uint8_t* dst, const uint16_t* src, size_t n) {
  u32a* d = static_cast<u32a*>(__builtin_assume_aligned(dst, 4));
  if ((((uintptr_t)src) & 3u) == 0) {  // fb 4-byte aligned (ps_malloc): two pixels per word
    const u32a* s = static_cast<const u32a*>(__builtin_assume_aligned(src, 4));
    for (size_t i = 0; i < n / 2; i++) {
      const uint32_t v = s[i];
      d[i] = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    }
  } else {
    for (size_t i = 0; i < n / 2; i++) {
      d[i] = (uint32_t)__builtin_bswap16(src[2 * i]) | ((uint32_t)__builtin_bswap16(src[2 * i + 1]) << 16);
    }
  }
}

void recordPush(int64_t t0, size_t bytes, bool full) {
  const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
  portENTER_CRITICAL(&g_statLock);
  g_stats.pushes++;
  g_stats.lastPushUs = us;
  if (us > g_stats.maxPushUs) g_stats.maxPushUs = us;
  if (full) g_stats.lastFullUs = us;
  g_stats.bytes += bytes;
  portEXIT_CRITICAL(&g_statLock);
}

// Rectangle already clipped and rounded (x1, y1 even; x2, y2 odd). Caller holds g_mu.
bool pushRect(const uint16_t* fb, int x1, int y1, int x2, int y2) {
  const int64_t t0 = esp_timer_get_time();
  setWindow(x1, y1, x2, y2);
  const int wpx = x2 - x1 + 1;
  int rowsLeft = y2 - y1 + 1;
  const uint16_t* row = fb + (size_t)y1 * W + x1;
  int col = 0;
  bool first = true, ok = true;
  int inflight = 0, cur = 0;
  cs(0);
  while (rowsLeft > 0) {
    if (inflight == 2) {  // the buffer about to be filled is the older one on the wire
      if (!waitOne()) ok = false;
      inflight--;
    }
    uint8_t* out = g_buf[cur];
    size_t fill = 0;  // pixels in this chunk
    while (fill < kChunkPx && rowsLeft > 0) {
      size_t n = kChunkPx - fill;
      if (n > (size_t)(wpx - col)) n = (size_t)(wpx - col);
      swapCopy(out + fill * 2, row + col, n);
      fill += n;
      col += (int)n;
      if (col == wpx) {
        col = 0;
        row += W;
        rowsLeft--;
      }
    }
    if (!queueChunk(cur, fill * 2, first)) {
      ok = false;
      break;
    }
    first = false;
    inflight++;
    cur ^= 1;
  }
  while (inflight > 0) {
    if (!waitOne()) ok = false;
    inflight--;
  }
  cs(1);
  recordPush(t0, (size_t)wpx * (size_t)(y2 - y1 + 1) * 2, x1 == 0 && y1 == 0 && x2 == W - 1 && y2 == H - 1);
  return ok;
}

// Whole panel in one color (round gauge lcdFillSolid). Both buffers hold the same data, so nothing is refilled.
bool fillSolid(uint16_t color) {
  const int64_t t0 = esp_timer_get_time();
  const uint32_t sw = __builtin_bswap16(color);
  for (int b = 0; b < 2; b++) {
    u32a* p = static_cast<u32a*>(__builtin_assume_aligned(g_buf[b], 4));
    for (size_t i = 0; i < kChunkPx / 2; i++) p[i] = sw | (sw << 16);
  }
  setWindow(0, 0, W - 1, H - 1);
  size_t left = (size_t)W * H;
  bool first = true, ok = true;
  int inflight = 0, cur = 0;
  cs(0);
  while (left > 0) {
    if (inflight == 2) {
      if (!waitOne()) ok = false;
      inflight--;
    }
    const size_t n = left < kChunkPx ? left : kChunkPx;
    if (!queueChunk(cur, n * 2, first)) {
      ok = false;
      break;
    }
    first = false;
    inflight++;
    cur ^= 1;
    left -= n;
  }
  while (inflight > 0) {
    if (!waitOne()) ok = false;
    inflight--;
  }
  cs(1);
  recordPush(t0, (size_t)W * H * 2, true);
  return ok;
}

void afterFrame() {
  if (g_firstDone) return;
  g_firstDone = true;
  sendBrightness(g_wantPct);
}

}  // namespace

bool displayBegin() {
  if (g_ok) return true;
  if (!g_mu) g_mu = xSemaphoreCreateMutex();
  if (!g_mu) return false;

  pinMode(board::LCD_CS, OUTPUT);
  digitalWrite(board::LCD_CS, HIGH);

  // Bus like the round gauge lcdBegin(): 4 data lines, GPIO matrix, DMA, chunk + 8 bytes per transfer
  spi_bus_config_t bus = {};
  bus.data0_io_num = board::LCD_D0;
  bus.data1_io_num = board::LCD_D1;
  bus.sclk_io_num = board::LCD_SCK;
  bus.data2_io_num = board::LCD_D2;
  bus.data3_io_num = board::LCD_D3;
  bus.data4_io_num = -1;
  bus.data5_io_num = -1;
  bus.data6_io_num = -1;
  bus.data7_io_num = -1;
  bus.max_transfer_sz = kChunk + 8;
  bus.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS;
  esp_err_t e = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
  if (e != ESP_OK) {
    noteErr(e);
    return false;
  }

  spi_device_interface_config_t dev = {};
  dev.command_bits = 8;
  dev.address_bits = 24;
  dev.mode = 0;  // demo spi_mode 0
  dev.clock_source = SPI_CLK_SRC_DEFAULT;
  dev.clock_speed_hz = kSpiHz;
  dev.spics_io_num = -1;  // CS by hand: one window = several transactions with CS low
  dev.flags = SPI_DEVICE_HALFDUPLEX;
  dev.queue_size = 2;  // two bounce buffers in flight
  e = spi_bus_add_device(SPI2_HOST, &dev, &g_spi);
  if (e != ESP_OK) {
    noteErr(e);
    return false;
  }
  spi_device_acquire_bus(g_spi, portMAX_DELAY);  // the panel is the only device on SPI2 (round gauge)
  int khz = 0;
  if (spi_device_get_actual_freq(g_spi, &khz) == ESP_OK) g_stats.spiKHz = (uint32_t)khz;

  for (int b = 0; b < 2; b++) {
    g_buf[b] = (uint8_t*)heap_caps_aligned_alloc(16, kChunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!g_buf[b]) return false;
  }

  // Hardware reset as esp_lcd_sh8601.c panel_sh8601_reset() (reset_active_high = 0): low 10 ms, high, wait 150 ms
  pinMode(board::LCD_RST, OUTPUT);
  digitalWrite(board::LCD_RST, LOW);
  delay(10);
  digitalWrite(board::LCD_RST, HIGH);
  delay(150);

  for (const InitCmd& c : kInit) {
    lcdCmd(c.cmd, c.data, c.len);
    if (c.delayMs) delay(c.delayMs);
  }
  // Black panel memory before the brightness comes up, so a first push smaller than the screen shows no random
  // pixels around it (not in the demo; only window + RAMWR, the same commands as every push)
  fillSolid(0x0000);

  portENTER_CRITICAL(&g_statLock);
  const bool spiOk = g_stats.spiError == 0;
  g_stats.pushes = 0;
  g_stats.bytes = 0;
  g_stats.maxPushUs = 0;
  g_stats.lastFullUs = 0;
  g_stats.ok = spiOk;
  portEXIT_CRITICAL(&g_statLock);
  g_ok = spiOk;
  return g_ok;
}

bool displayPush(const uint16_t* fb, int x, int y, int w, int h) {
  if (!g_ok || !fb || w <= 0 || h <= 0) return false;
  int x1 = x, y1 = y, x2 = x + w - 1, y2 = y + h - 1;
  if (x1 < 0) x1 = 0;
  if (y1 < 0) y1 = 0;
  if (x2 > W - 1) x2 = W - 1;
  if (y2 > H - 1) y2 = H - 1;
  if (x1 > x2 || y1 > y2) return true;  // fully off screen: nothing to send
  // demo example_lvgl_rounder_cb: start rounded down to even, end rounded up to odd (599 and 449 are odd)
  x1 &= ~1;
  y1 &= ~1;
  x2 |= 1;
  y2 |= 1;
  xSemaphoreTake(g_mu, portMAX_DELAY);
  const bool ok = pushRect(fb, x1, y1, x2, y2);
  if (ok) afterFrame();
  xSemaphoreGive(g_mu);
  return ok;
}

bool displayFill(uint16_t rgb565) {
  if (!g_ok) return false;
  xSemaphoreTake(g_mu, portMAX_DELAY);
  const bool ok = fillSolid(rgb565);
  if (ok) afterFrame();
  xSemaphoreGive(g_mu);
  return ok;
}

void displayBrightness(uint8_t pct) {
  if (pct > 100) pct = 100;
  if (!g_mu) {  // before displayBegin(): just remember it
    g_wantPct = pct;
    return;
  }
  xSemaphoreTake(g_mu, portMAX_DELAY);
  g_wantPct = pct;
  if (g_ok && g_firstDone) sendBrightness(pct);
  xSemaphoreGive(g_mu);
}

void displayStats(DisplayStats* out, bool resetMax) {
  if (!out) return;
  portENTER_CRITICAL(&g_statLock);
  *out = g_stats;
  if (resetMax) g_stats.maxPushUs = 0;
  portEXIT_CRITICAL(&g_statLock);
}

}  // namespace hw
