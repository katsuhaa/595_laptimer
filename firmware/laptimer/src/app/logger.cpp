// logger.cpp - see logger.h
#include "logger.h"

#include <FS.h>
#include <string.h>

#include "../hw/sdcard.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

namespace logger {
namespace {

// Stream buffer sizes (PSRAM). Rates: main 4.5 KB/s, vbo 2.8 KB/s, ubx 2.5 KB/s, imu 10 KB/s, can up to 35 KB/s.
// Each buffer holds several seconds so a slow card write (or the web module holding the lock) loses nothing.
constexpr size_t kBufSize[S_COUNT] = {128 * 1024, 64 * 1024, 64 * 1024, 256 * 1024, 256 * 1024};
constexpr size_t kChunk = 16 * 1024;     // write unit
constexpr uint32_t kSyncMs = 1000;       // fsync period
constexpr uint32_t kLockMs = 2000;       // wait for the storage lock
const char* const kSuffix[S_COUNT] = {".csv", ".vbo", "_raw.ubx", "_imu.csv", "_can.csv"};

enum CtrlType : uint8_t { C_OPEN, C_CLOSE, C_LAP, C_META, C_BEST };
struct Ctrl {
  CtrlType type;
  char cid[32];
  uint8_t* data;  // heap copy (PSRAM), freed by the task
  size_t n;
};

StreamBufferHandle_t g_sb[S_COUNT] = {};
StaticStreamBuffer_t g_sbStatic[S_COUNT];
QueueHandle_t g_ctrl = nullptr;
SemaphoreHandle_t g_statMu = nullptr;
Stats g_stats = {};
volatile bool g_open = false;     // set by requestOpen, cleared when the close is done
volatile bool g_accept = false;   // producers may write
OpenInfo g_pending = {};          // copied by requestOpen
uint8_t* g_chunk = nullptr;       // internal RAM, kChunk bytes

fs::File g_file[S_COUNT];
fs::File g_laps;
char g_dir[48];
char g_base[96];  // <dir>/<id>_<cid>
uint32_t g_lastSync = 0;

void statErr(const char* e) {
  xSemaphoreTake(g_statMu, portMAX_DELAY);
  g_stats.errors++;
  strncpy(g_stats.lastError, e, sizeof g_stats.lastError - 1);
  xSemaphoreGive(g_statMu);
}

uint8_t* copyOf(const void* d, size_t n) {
  uint8_t* p = static_cast<uint8_t*>(heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!p) p = static_cast<uint8_t*>(malloc(n + 1));
  if (!p) return nullptr;
  memcpy(p, d, n);
  p[n] = 0;
  return p;
}

bool post(CtrlType t, const char* cid, const void* d, size_t n) {
  Ctrl c = {};
  c.type = t;
  if (cid) strncpy(c.cid, cid, sizeof c.cid - 1);
  if (d && n) {
    c.data = copyOf(d, n);
    if (!c.data) return false;
    c.n = n;
  }
  if (xQueueSend(g_ctrl, &c, 0) != pdTRUE) {
    free(c.data);
    return false;
  }
  return true;
}

// The storage lock, waiting up to `tries` x kLockMs (the web module or a session scan may hold it a while)
bool lockWait(int tries) {
  for (int i = 0; i < tries; ++i) {
    if (hw::sdLock(kLockMs)) return true;
  }
  return false;
}

// Writes the whole buffer under the storage lock (short sections)
bool writeLocked(fs::File& f, const uint8_t* d, size_t n) {
  if (!f) return false;
  if (!lockWait(3)) {  // the chunk is already out of its stream buffer: wait rather than drop it
    statErr("storage lock timeout");
    return false;
  }
  const uint32_t t0 = millis();
  const size_t w = f.write(d, n);
  const uint32_t dt = millis() - t0;
  hw::sdUnlock();
  xSemaphoreTake(g_statMu, portMAX_DELAY);
  if (dt > g_stats.maxWriteMs) g_stats.maxWriteMs = dt;
  xSemaphoreGive(g_statMu);
  if (w != n) {
    statErr("write failed");
    return false;
  }
  return true;
}

// Replaces a small file atomically: write <path>.tmp, then rename
bool replaceFile(const char* path, const uint8_t* d, size_t n) {
  char tmp[72];
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  if (!hw::sdLock(kLockMs)) return false;
  fs::FS& fs = hw::sd();
  bool ok = false;
  fs::File f = fs.open(tmp, FILE_WRITE);
  if (f) {
    ok = f.write(d, n) == n;
    f.flush();
    f.close();
  }
  if (ok) {
    fs.remove(path);
    ok = fs.rename(tmp, path);
  }
  hw::sdUnlock();
  if (!ok) statErr("replace failed");
  return ok;
}

void drain(int s, bool all) {
  if (!g_sb[s]) return;
  for (;;) {
    const size_t avail = xStreamBufferBytesAvailable(g_sb[s]);
    if (avail == 0 || (!all && avail < kChunk)) return;
    const size_t n = xStreamBufferReceive(g_sb[s], g_chunk, kChunk, 0);
    if (n == 0) return;
    if (writeLocked(g_file[s], g_chunk, n)) {
      xSemaphoreTake(g_statMu, portMAX_DELAY);
      g_stats.bytes[s] += n;
      xSemaphoreGive(g_statMu);
    }
  }
}

void syncAll() {
  if (!hw::sdLock(kLockMs)) return;
  for (int s = 0; s < S_COUNT; ++s) {
    if (g_file[s]) g_file[s].flush();
  }
  if (g_laps) g_laps.flush();
  hw::sdUnlock();
  xSemaphoreTake(g_statMu, portMAX_DELAY);
  g_stats.syncs++;
  xSemaphoreGive(g_statMu);
}

void writeText(fs::File& f, const char* t) {
  if (t && *t) writeLocked(f, reinterpret_cast<const uint8_t*>(t), strlen(t));
}

void doOpen() {
  const OpenInfo& o = g_pending;
  snprintf(g_dir, sizeof g_dir, "/sessions/%s", o.id);
  snprintf(g_base, sizeof g_base, "%s/%s_%s", g_dir, o.id, o.cid);
  bool ok = lockWait(5);
  if (ok) {
    fs::FS& fs = hw::sd();
    fs.mkdir("/sessions");
    fs.mkdir(g_dir);
    char path[96];
    for (int s = 0; s < S_COUNT; ++s) {
      if (s == S_CAN && !o.canRaw) continue;
      snprintf(path, sizeof path, "%s%s", g_base, kSuffix[s]);
      g_file[s] = fs.open(path, FILE_WRITE);
      if (!g_file[s]) ok = false;
    }
    snprintf(path, sizeof path, "%s_laps.csv", g_base);
    g_laps = fs.open(path, FILE_WRITE);
    if (!g_laps) ok = false;
    hw::sdUnlock();
  }
  if (!ok) statErr("session open failed");
  writeText(g_file[S_MAIN], o.mainHeader);
  writeText(g_file[S_VBO], o.vboHeader);
  writeText(g_file[S_IMU], o.imuHeader);
  if (o.canRaw) writeText(g_file[S_CAN], o.canHeader);
  writeText(g_laps, o.lapsHeader);
  if (o.metaJson) {
    char path[72];
    snprintf(path, sizeof path, "%s/meta.json", g_dir);
    replaceFile(path, reinterpret_cast<const uint8_t*>(o.metaJson), strlen(o.metaJson));
  }
  syncAll();
  g_lastSync = millis();
  xSemaphoreTake(g_statMu, portMAX_DELAY);
  g_stats.open = true;
  strncpy(g_stats.id, o.id, sizeof g_stats.id - 1);
  memset(g_stats.bytes, 0, sizeof g_stats.bytes);
  memset(g_stats.drops, 0, sizeof g_stats.drops);
  xSemaphoreGive(g_statMu);
  for (int s = 0; s < S_COUNT; ++s) xStreamBufferReset(g_sb[s]);
  g_accept = true;
}

void doClose(const Ctrl& c) {
  g_accept = false;
  for (int s = 0; s < S_COUNT; ++s) drain(s, true);
  if (c.data) {
    char path[72];
    snprintf(path, sizeof path, "%s/meta.json", g_dir);
    replaceFile(path, c.data, c.n);
  }
  // the files must be closed (their directory entries hold the final sizes): wait for the card as long as it takes
  while (!lockWait(5)) statErr("storage lock timeout (close)");
  for (int s = 0; s < S_COUNT; ++s) {
    if (g_file[s]) {
      g_file[s].flush();
      g_file[s].close();
    }
  }
  if (g_laps) {
    g_laps.flush();
    g_laps.close();
  }
  hw::sdUnlock();
  xSemaphoreTake(g_statMu, portMAX_DELAY);
  g_stats.open = false;
  xSemaphoreGive(g_statMu);
  g_open = false;
}

}  // namespace

void begin() {
  g_statMu = xSemaphoreCreateMutex();
  g_ctrl = xQueueCreate(24, sizeof(Ctrl));
  g_chunk = static_cast<uint8_t*>(heap_caps_malloc(kChunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  for (int s = 0; s < S_COUNT; ++s) {
    uint8_t* store = static_cast<uint8_t*>(heap_caps_malloc(kBufSize[s] + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (store) g_sb[s] = xStreamBufferCreateStatic(kBufSize[s], 1, store, &g_sbStatic[s]);
  }
}

bool requestOpen(const OpenInfo& o) {
  if (g_open) return false;
  g_pending = o;
  g_open = true;
  if (!post(C_OPEN, o.cid, nullptr, 0)) {
    g_open = false;
    return false;
  }
  return true;
}

void requestClose(const char* finalMetaJson) {
  if (!g_open) return;
  g_accept = false;  // stop producers right away; the task drains what is already buffered
  // The close must reach the task (else the logger stays "open" and no session could start again):
  // without the final meta.json if there is no memory for its copy, and waiting for a queue slot
  if (post(C_CLOSE, nullptr, finalMetaJson, finalMetaJson ? strlen(finalMetaJson) : 0)) return;
  Ctrl c = {};
  c.type = C_CLOSE;
  xQueueSend(g_ctrl, &c, portMAX_DELAY);
}

bool isOpen() { return g_open; }

void write(Stream s, const void* data, size_t n) {
  if (!g_accept || s >= S_COUNT || !g_sb[s] || n == 0) return;
  if (xStreamBufferSpacesAvailable(g_sb[s]) < n) {
    xSemaphoreTake(g_statMu, portMAX_DELAY);
    g_stats.drops[s] += n;
    xSemaphoreGive(g_statMu);
    return;
  }
  xStreamBufferSend(g_sb[s], data, n, 0);
}

void lapLine(const char* line) {
  if (g_open && line) post(C_LAP, nullptr, line, strlen(line));
}
void meta(const char* json) {
  if (g_open && json) post(C_META, nullptr, json, strlen(json));
}
void bestLap(const char* cid, const uint8_t* data, size_t n) { post(C_BEST, cid, data, n); }

void stats(Stats* out) {
  xSemaphoreTake(g_statMu, portMAX_DELAY);
  *out = g_stats;
  xSemaphoreGive(g_statMu);
}

void task(void*) {
  for (;;) {
    Ctrl c;
    while (xQueueReceive(g_ctrl, &c, pdMS_TO_TICKS(g_accept ? 20 : 100)) == pdTRUE) {
      switch (c.type) {
        case C_OPEN:
          doOpen();
          break;
        case C_CLOSE:
          doClose(c);
          break;
        case C_LAP:
          if (g_laps) writeLocked(g_laps, c.data, c.n);
          break;
        case C_META: {
          char path[72];
          snprintf(path, sizeof path, "%s/meta.json", g_dir);
          replaceFile(path, c.data, c.n);
          break;
        }
        case C_BEST: {
          char path[64];
          if (hw::sdLock(kLockMs)) {
            hw::sd().mkdir("/best");
            hw::sdUnlock();
          }
          snprintf(path, sizeof path, "/best/%s.bin", c.cid);
          replaceFile(path, c.data, c.n);
          break;
        }
      }
      free(c.data);
    }
    if (g_accept) {
      for (int s = 0; s < S_COUNT; ++s) drain(s, false);
      if (millis() - g_lastSync >= kSyncMs) {
        for (int s = 0; s < S_COUNT; ++s) drain(s, true);
        syncAll();
        g_lastSync = millis();
      }
    }
  }
}

}  // namespace logger
