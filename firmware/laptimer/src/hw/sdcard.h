// sdcard.h - XTSD (SD NAND, SPI mode) on the TF-slot nets, FAT through the Arduino SD library, mounted at /sd.
//
// SPI3_HOST (Arduino HSPI on the ESP32-S3) on board::SD_SCK / SD_MISO / SD_MOSI, CS board::SD_CS (software CS of the
// SD library). SPI2_HOST belongs to the display. Pins and host as the Waveshare demo 04_SD_Card (SD_Read_Mode USER_SPI).
// Mount: 20 MHz first (the demo's SDSPI_HOST_DEFAULT() speed), then 40 MHz is tried and kept if it mounts and a
// write/read check passes. NOTE: the SD library of arduino-esp32 3.3.11 caps the clock at 25 MHz
// (sd_diskio.cpp ff_sd_initialize) and the SPI HAL then picks 80 MHz / 4 = 20 MHz, so both steps run at 20 MHz;
// SdInfo.spiHz shows the clock really used. max_files 12 (lead: 6 session files + web + meta/best rewrites).
// Never formats (format_if_empty = false).
//
// Storage mutex: recursive, shared by the logger and the web module. Hold it around every FS call sequence that must
// not interleave with the other side; keep sections short (the logger waits on it).
#pragma once
#include <FS.h>
#include <stdint.h>

namespace hw {

struct SdInfo {
  bool mounted;
  uint8_t cardType;      // sdcard_type_t of SD.cardType(): 0 none, 1 MMC, 2 SD, 3 SDHC/SDXC, 4 unknown
  uint32_t requestedHz;  // frequency given to SD.begin() for the mount in use
  uint32_t spiHz;        // SPI clock really used (SPI clock divider read back after the mount)
  bool fastTried;        // the 40 MHz step was tried
  bool fastKept;         // ...and kept
  uint64_t cardBytes;    // SD.cardSize()
};

bool sdBegin();     // call once from setup(); creates the storage mutex even if the card does not mount
bool sdMounted();
bool sdFormatIfEmpty();  // owner's explicit request only: FAT is made only when the card has none
fs::FS& sd();       // the SD file system (paths relative to /sd, e.g. "/sessions")
uint64_t sdTotalBytes();  // FAT volume size; 0 if not mounted
uint64_t sdUsedBytes();   // first call after the mount may scan the FAT (slow, under the storage mutex)

bool sdLock(uint32_t timeoutMs);  // storage mutex (recursive); false = timeout or sdBegin() not called
void sdUnlock();

// Boot self-test: writes 1 MiB (1,048,576 bytes, pseudo-random) to /selftest.bin, reads it back and compares, then
// deletes the file. Rates in MiB/s (open..close, so the flush is included). false = not mounted, lock timeout, I/O
// error or mismatch; failReason (optional) gets a short English reason.
bool sdSelfTest(float* writeMBps, float* readMBps, const char** failReason = nullptr);

void sdInfo(SdInfo* out);

}  // namespace hw
