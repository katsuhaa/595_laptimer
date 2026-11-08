// sdcard.cpp - see sdcard.h
#include "sdcard.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <string.h>

#include "../board.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace hw {
namespace {

constexpr uint32_t kHzStart = 20 * 1000 * 1000;  // demo 04_SD_Card: SDSPI_HOST_DEFAULT() = SDMMC_FREQ_DEFAULT (20 MHz)
constexpr uint32_t kHzFast = 40 * 1000 * 1000;   // tried after 20 MHz works (lead's request)
const char* const kMount = "/sd";
constexpr uint8_t kMaxFiles = 12;  // lead: logger 6 session files + web 1-2 + meta.json/best rewrites
constexpr uint32_t kApiLockMs = 5000;
constexpr uint32_t kSelfTestLockMs = 10000;
const char* const kSelfTestPath = "/selftest.bin";
const char* const kCheckPath = "/sdcheck.tmp";  // short write/read check of the speed steps (deleted again)
constexpr size_t kBlock = 4096;
constexpr size_t kSelfTestBytes = 1024 * 1024;
constexpr size_t kCheckBytes = 16 * 1024;

// SPI3_HOST: esp32-hal-spi.h "HSPI 1 // ESP32S2, S3, P4 - SPI 3 bus" (SPI2_HOST = FSPI is used by the display)
SPIClass g_spi(HSPI);
SemaphoreHandle_t g_mu = nullptr;
SdInfo g_info = {};
portMUX_TYPE g_infoLock = portMUX_INITIALIZER_UNLOCKED;
uint8_t* g_blk = nullptr;  // kBlock bytes, internal RAM

uint32_t nextRand(uint32_t* s) {  // xorshift32
  uint32_t x = *s;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *s = x;
  return x;
}

void fillPattern(uint8_t* b, size_t n, uint32_t* s) {
  for (size_t i = 0; i + 4 <= n; i += 4) {
    const uint32_t v = nextRand(s);
    memcpy(b + i, &v, 4);
  }
}

bool samePattern(const uint8_t* b, size_t n, uint32_t* s) {
  for (size_t i = 0; i + 4 <= n; i += 4) {
    const uint32_t v = nextRand(s);
    if (memcmp(b + i, &v, 4) != 0) return false;
  }
  return true;
}

// Writes `bytes` of pattern to path, reads them back and compares, deletes the file. Caller holds the storage mutex.
bool writeReadCheck(const char* path, size_t bytes, uint32_t seed, int64_t* writeUs, int64_t* readUs, const char** why) {
  const int64_t t0 = esp_timer_get_time();
  fs::File f = SD.open(path, FILE_WRITE);
  if (!f) {
    *why = "open for write failed";
    return false;
  }
  uint32_t s = seed;
  for (size_t done = 0; done < bytes; done += kBlock) {
    fillPattern(g_blk, kBlock, &s);
    if (f.write(g_blk, kBlock) != kBlock) {
      f.close();
      SD.remove(path);
      *why = "write failed (card full?)";
      return false;
    }
  }
  f.close();  // flush + FAT update inside the timing
  const int64_t t1 = esp_timer_get_time();
  f = SD.open(path, FILE_READ);
  if (!f) {
    SD.remove(path);
    *why = "open for read failed";
    return false;
  }
  bool ok = f.size() == bytes;
  if (!ok) *why = "size differs after write";
  s = seed;
  for (size_t done = 0; ok && done < bytes; done += kBlock) {
    if (f.read(g_blk, kBlock) != kBlock) {
      ok = false;
      *why = "read failed";
    } else if (!samePattern(g_blk, kBlock, &s)) {
      ok = false;
      *why = "data read back differs";
    }
  }
  f.close();
  const int64_t t2 = esp_timer_get_time();
  SD.remove(path);
  *writeUs = t1 - t0;
  *readUs = t2 - t1;
  return ok;
}

bool quickCheck() {
  int64_t w, r;
  const char* why = "";
  return writeReadCheck(kCheckPath, kCheckBytes, 0x12345678u, &w, &r, &why);
}

bool mountAt(uint32_t hz) {
  // SDFS::begin(ssPin, spi, frequency, mountpoint, max_files, format_if_empty): spi.begin() inside is a no-op
  // because g_spi was started with the board pins
  if (!SD.begin((uint8_t)board::SD_CS, g_spi, hz, kMount, kMaxFiles, false)) return false;
  const uint32_t used = spiClockDivToFrequency(nullptr, g_spi.getClockDivider());  // divider of the last transfer
  const uint8_t type = (uint8_t)SD.cardType();
  const uint64_t size = SD.cardSize();
  portENTER_CRITICAL(&g_infoLock);
  g_info.requestedHz = hz;
  g_info.spiHz = used;
  g_info.cardType = type;
  g_info.cardBytes = size;
  portEXIT_CRITICAL(&g_infoLock);
  return true;
}

float mibPerSec(size_t bytes, int64_t us) {
  if (us <= 0) return 0.0f;
  return ((float)bytes / 1048576.0f) / ((float)us / 1000000.0f);
}

}  // namespace

bool sdBegin() {
  if (!g_mu) g_mu = xSemaphoreCreateRecursiveMutex();
  if (!g_mu) return false;
  if (!sdLock(kApiLockMs)) return false;
  if (g_info.mounted) {
    sdUnlock();
    return true;
  }
  if (!g_blk) g_blk = (uint8_t*)heap_caps_malloc(kBlock, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  bool ok = g_blk != nullptr && g_spi.begin(board::SD_SCK, board::SD_MISO, board::SD_MOSI, -1) && mountAt(kHzStart);
  bool tried = false, kept = false;
  if (ok && quickCheck()) {
    tried = true;
    SD.end();
    if (mountAt(kHzFast) && quickCheck()) {
      kept = true;
    } else {
      SD.end();
      ok = mountAt(kHzStart);
    }
  }
  portENTER_CRITICAL(&g_infoLock);
  g_info.mounted = ok;
  g_info.fastTried = tried;
  g_info.fastKept = kept && ok;
  if (!ok) {
    g_info.requestedHz = 0;
    g_info.spiHz = 0;
    g_info.cardType = 0;
    g_info.cardBytes = 0;
  }
  portEXIT_CRITICAL(&g_infoLock);
  sdUnlock();
  return ok;
}

bool sdMounted() {
  return g_info.mounted;
}

// Only on the owner's explicit request (web page button with confirm): mount with format_if_empty = true,
// which makes a FAT only when the card has no file system (FR_NO_FILESYSTEM). Never called automatically.
bool sdFormatIfEmpty() {
  if (!g_mu || !sdLock(kApiLockMs)) return false;
  if (g_info.mounted) {
    sdUnlock();
    return true;
  }
  SD.end();
  bool ok = SD.begin((uint8_t)board::SD_CS, g_spi, kHzStart, kMount, kMaxFiles, true);
  if (ok) {
    SD.end();
    ok = mountAt(kHzStart);
  }
  portENTER_CRITICAL(&g_infoLock);
  g_info.mounted = ok;
  portEXIT_CRITICAL(&g_infoLock);
  sdUnlock();
  return ok;
}

fs::FS& sd() {
  return SD;
}

uint64_t sdTotalBytes() {
  if (!g_info.mounted || !sdLock(kApiLockMs)) return 0;
  const uint64_t v = SD.totalBytes();
  sdUnlock();
  return v;
}

uint64_t sdUsedBytes() {
  if (!g_info.mounted || !sdLock(kApiLockMs)) return 0;
  const uint64_t v = SD.usedBytes();
  sdUnlock();
  return v;
}

bool sdLock(uint32_t timeoutMs) {
  if (!g_mu) return false;
  return xSemaphoreTakeRecursive(g_mu, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void sdUnlock() {
  if (g_mu) xSemaphoreGiveRecursive(g_mu);
}

bool sdSelfTest(float* writeMBps, float* readMBps, const char** failReason) {
  const char* why = "ok";
  int64_t w = 0, r = 0;
  bool ok = false;
  if (!g_info.mounted || !g_blk) {
    why = "not mounted";
  } else if (!sdLock(kSelfTestLockMs)) {
    why = "storage lock timeout";
  } else {
    ok = writeReadCheck(kSelfTestPath, kSelfTestBytes, 0x5EED2026u, &w, &r, &why);
    sdUnlock();
  }
  if (writeMBps) *writeMBps = ok ? mibPerSec(kSelfTestBytes, w) : 0.0f;
  if (readMBps) *readMBps = ok ? mibPerSec(kSelfTestBytes, r) : 0.0f;
  if (failReason) *failReason = why;
  return ok;
}

void sdInfo(SdInfo* out) {
  if (!out) return;
  portENTER_CRITICAL(&g_infoLock);
  *out = g_info;
  portEXIT_CRITICAL(&g_infoLock);
}

}  // namespace hw
