// nvs.cpp - see nvs.h
#include "nvs.h"

#include <Preferences.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace hw {
namespace {

const char* const kNamespace = "laptimer";
constexpr uint32_t kMagic = 0x564E544Cu;  // "LTNV"
constexpr size_t kMaxKey = 15;            // NVS key length limit (Preferences::getType rejects longer keys)

struct Header {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  uint32_t size;
  uint32_t crc;
};
static_assert(sizeof(Header) == 16, "header layout");

Preferences g_prefs;
bool g_open = false;
SemaphoreHandle_t g_mu = nullptr;
portMUX_TYPE g_initLock = portMUX_INITIALIZER_UNLOCKED;

uint32_t crc32(const uint8_t* p, size_t n) {  // CRC-32 (IEEE 802.3, reflected 0xEDB88320)
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

bool keyOk(const char* key) {
  return key && key[0] && strlen(key) <= kMaxKey;
}

// Takes the mutex and opens the namespace on first use. Arduino's initArduino() has already run nvs_flash_init().
bool enter() {
  if (!g_mu) {
    SemaphoreHandle_t m = xSemaphoreCreateMutex();
    if (!m) return false;
    portENTER_CRITICAL(&g_initLock);
    if (!g_mu) {
      g_mu = m;
      m = nullptr;
    }
    portEXIT_CRITICAL(&g_initLock);
    if (m) vSemaphoreDelete(m);
  }
  xSemaphoreTake(g_mu, portMAX_DELAY);
  if (!g_open) g_open = g_prefs.begin(kNamespace, false);
  if (!g_open) xSemaphoreGive(g_mu);
  return g_open;
}

void leave() {
  xSemaphoreGive(g_mu);
}

// Reads the stored blob of key into a new buffer (malloc). Caller holds the mutex. nullptr = absent.
uint8_t* readBlob(const char* key, size_t* len) {
  *len = 0;
  if (!g_prefs.isKey(key)) return nullptr;  // avoids the library's error log for a missing key (round gauge does this)
  const size_t n = g_prefs.getBytesLength(key);
  if (n == 0) return nullptr;
  uint8_t* b = (uint8_t*)malloc(n);
  if (!b) return nullptr;
  if (g_prefs.getBytes(key, b, n) != n) {
    free(b);
    return nullptr;
  }
  *len = n;
  return b;
}

// Checks a stored blob against the expected size/version; returns the payload or nullptr
const uint8_t* payloadOf(const uint8_t* blob, size_t len, size_t size, uint16_t version) {
  if (!blob || len != sizeof(Header) + size) return nullptr;
  Header h;
  memcpy(&h, blob, sizeof h);
  if (h.magic != kMagic || h.version != version || h.size != size) return nullptr;
  const uint8_t* p = blob + sizeof(Header);
  if (h.crc != crc32(p, size)) return nullptr;
  return p;
}

}  // namespace

bool nvsLoad(const char* key, void* data, size_t size, uint16_t version) {
  if (!keyOk(key) || !data || size == 0 || !enter()) return false;
  size_t len = 0;
  uint8_t* blob = readBlob(key, &len);
  const uint8_t* p = payloadOf(blob, len, size, version);
  if (p) memcpy(data, p, size);
  free(blob);
  leave();
  return p != nullptr;
}

bool nvsSave(const char* key, const void* data, size_t size, uint16_t version) {
  if (!keyOk(key) || !data || size == 0) return false;
  const size_t total = sizeof(Header) + size;
  uint8_t* buf = (uint8_t*)malloc(total);
  if (!buf) return false;
  Header h = {kMagic, version, 0, (uint32_t)size, crc32((const uint8_t*)data, size)};
  memcpy(buf, &h, sizeof h);
  memcpy(buf + sizeof h, data, size);
  if (!enter()) {
    free(buf);
    return false;
  }
  size_t len = 0;
  uint8_t* old = readBlob(key, &len);
  bool ok = old && len == total && memcmp(old, buf, total) == 0;  // already stored: no flash write
  free(old);
  if (!ok && g_prefs.putBytes(key, buf, total) == total) {
    uint8_t* back = readBlob(key, &len);
    ok = back && len == total && memcmp(back, buf, total) == 0;
    free(back);
  }
  leave();
  free(buf);
  return ok;
}

bool nvsErase(const char* key) {
  if (!keyOk(key) || !enter()) return false;
  const bool ok = !g_prefs.isKey(key) || g_prefs.remove(key);
  leave();
  return ok;
}

}  // namespace hw
