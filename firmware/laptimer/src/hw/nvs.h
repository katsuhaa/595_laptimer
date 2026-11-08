// nvs.h - small opaque blobs in NVS (Preferences, namespace "laptimer"), one blob per key.
//
// Each blob is stored with a 16-byte header {magic, version, size, CRC-32}; nvsLoad() only succeeds if the stored size
// equals `size`, the version equals `version` and the CRC matches, so a struct whose layout changed reads as "absent"
// (the caller keeps its defaults). nvsSave() writes, reads back and compares (round gauge platSaveSettings: "保存して
// 読み返し、合っていれば true") and skips the flash write when the stored blob is already identical.
// Keys: at most 15 characters (NVS limit). Partition app3M_fat9M_16MB: nvs = 20 KB at 0x9000.
// Thread-safe (one mutex). A flash write stalls code running from flash on both cores for a few ms (the round gauge
// saw a few UART bytes lost then): save when it does not matter (setting changed, stopped, session closed).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace hw {

bool nvsLoad(const char* key, void* data, size_t size, uint16_t version = 0);  // false = absent/mismatch; data untouched
bool nvsSave(const char* key, const void* data, size_t size, uint16_t version = 0);
bool nvsErase(const char* key);  // true if the key is gone afterwards

}  // namespace hw
