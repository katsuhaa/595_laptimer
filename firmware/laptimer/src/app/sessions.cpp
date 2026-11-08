// sessions.cpp - see sessions.h
#include "sessions.h"

#include <FS.h>
#include <stdlib.h>
#include <string.h>

#include "../hw/sdcard.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lt_core.h"
#include "state.h"

namespace sessions {
namespace {

Info* g_list = nullptr;
int g_n = 0;
SemaphoreHandle_t g_mu = nullptr;

struct FileSource : lt::json::Source {
  explicit FileSource(fs::File& f) : f_(f) {}
  int read(uint8_t* buf, int cap) override { return static_cast<int>(f_.read(buf, static_cast<size_t>(cap))); }
  fs::File& f_;
};

// Reads the fields of meta.json the list needs (ARCH §5.3); unknown members are skipped
void readMeta(fs::File& f, Info* s) {
  FileSource src(f);
  lt::json::Reader r(src);
  if (!r.beginObject()) return;
  while (r.nextKey()) {
    if (r.keyIs("start_utc_ms")) {
      r.readInt64(&s->startUtcMs);
    } else if (r.keyIs("end_utc_ms")) {
      r.readInt64(&s->endUtcMs);
    } else if (r.keyIs("laps")) {
      int32_t v = 0;
      if (r.readInt(&v)) s->laps = static_cast<int16_t>(v);
    } else if (r.keyIs("best_ms")) {
      int32_t v = -1;
      if (r.readInt(&v)) s->bestMs = v;
    } else if (r.keyIs("closed")) {
      r.readBool(&s->closed);
    } else if (r.keyIs("course") && r.peek() == lt::json::Type::Object) {
      r.beginObject();
      while (r.nextKey()) {
        if (r.keyIs("id")) {
          r.readString(s->cid, sizeof s->cid);
        } else if (r.keyIs("name")) {
          r.readString(s->name, sizeof s->name);
        } else {
          r.skip();
        }
      }
    } else {
      r.skip();
    }
    if (!r.ok()) return;
  }
}

int cmpNewest(const void* a, const void* b) {
  return strcmp(static_cast<const Info*>(b)->id, static_cast<const Info*>(a)->id);  // ids are sortable times
}

bool isMainFile(const char* name, const char* id) {
  // <id>_<cid>.csv or .vbo (not _imu.csv/_can.csv/_laps.csv)
  const size_t n = strlen(name);
  if (strncmp(name, id, strlen(id)) != 0) return false;
  if (n > 4 && strcmp(name + n - 4, ".vbo") == 0) return true;
  if (n > 4 && strcmp(name + n - 4, ".csv") == 0) {
    return !strstr(name, "_imu.csv") && !strstr(name, "_can.csv") && !strstr(name, "_laps.csv");
  }
  return false;
}

}  // namespace

void begin() {
  g_mu = xSemaphoreCreateMutex();
  g_list = static_cast<Info*>(heap_caps_calloc(kMax, sizeof(Info), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

int rescan() {
  if (!g_list) return 0;
  app::PsBuf buf(sizeof(Info) * kMax);
  if (!buf.ok()) return g_n;
  Info* tmp = buf.as<Info>();
  int n = 0;
  // The storage lock is taken per session folder (the logger may be writing): a scan of many sessions
  // never holds the card for long
  if (!hw::sdLock(5000)) return g_n;
  fs::File root = hw::sd().open("/sessions");
  const bool dirOk = root && root.isDirectory();
  hw::sdUnlock();
  while (dirOk && n < kMax) {
    if (!hw::sdLock(5000)) break;
    fs::File d = root.openNextFile();
    if (!d) {
      hw::sdUnlock();
      break;
    }
    if (d.isDirectory()) {
      Info s = {};
      strncpy(s.id, d.name(), sizeof s.id - 1);
      s.bestMs = -1;
      strcpy(s.cid, "street");
      bool dl = false;
      for (fs::File f = d.openNextFile(); f; f = d.openNextFile()) {
        const char* name = f.name();
        if (strncmp(name, ".dl_", 4) == 0) {
          if (strcmp(name + 4, "report.html") == 0 || isMainFile(name + 4, s.id)) dl = true;
          continue;
        }
        s.bytes += f.size();
        if (strcmp(name, "meta.json") == 0) readMeta(f, &s);
      }
      s.downloaded = dl;
      tmp[n++] = s;
    }
    d.close();
    hw::sdUnlock();
  }
  if (root && hw::sdLock(5000)) {
    root.close();
    hw::sdUnlock();
  }
  qsort(tmp, static_cast<size_t>(n), sizeof(Info), cmpNewest);
  xSemaphoreTake(g_mu, portMAX_DELAY);
  memcpy(g_list, tmp, sizeof(Info) * static_cast<size_t>(n));
  g_n = n;
  xSemaphoreGive(g_mu);
  return n;
}

int count() { return g_n; }

bool get(int i, Info* out) {
  if (!g_list || i < 0) return false;
  xSemaphoreTake(g_mu, portMAX_DELAY);
  const bool ok = i < g_n;
  if (ok) *out = g_list[i];
  xSemaphoreGive(g_mu);
  return ok;
}

int undownloaded() {
  int k = 0;
  xSemaphoreTake(g_mu, portMAX_DELAY);
  for (int i = 0; i < g_n; ++i) k += g_list[i].downloaded ? 0 : 1;
  xSemaphoreGive(g_mu);
  return k;
}

uint64_t totalBytes() {
  uint64_t b = 0;
  xSemaphoreTake(g_mu, portMAX_DELAY);
  for (int i = 0; i < g_n; ++i) b += g_list[i].bytes;
  xSemaphoreGive(g_mu);
  return b;
}

bool removeSession(const char* id) {
  if (!id || !id[0] || strchr(id, '/') || strstr(id, "..")) return false;
  char dir[48];
  snprintf(dir, sizeof dir, "/sessions/%s", id);
  fs::FS& fs = hw::sd();
  bool ok = true;
  // Collect the names first, then delete (deleting while iterating a FAT directory can skip entries).
  // The storage lock is taken per step: deleting a large session does not hold up the logger.
  for (int round = 0; round < 8; ++round) {
    char names[16][64];
    int k = 0;
    if (!hw::sdLock(5000)) return false;
    fs::File d = fs.open(dir);
    const bool isDir = d && d.isDirectory();
    if (isDir) {
      for (fs::File f = d.openNextFile(); f && k < 16; f = d.openNextFile()) {
        strncpy(names[k], f.name(), sizeof names[k] - 1);
        names[k][sizeof names[k] - 1] = 0;
        ++k;
      }
    }
    if (d) d.close();
    hw::sdUnlock();
    if (!isDir) {
      ok = round > 0;
      break;
    }
    char path[96];
    for (int i = 0; i < k; ++i) {
      snprintf(path, sizeof path, "%s/%s", dir, names[i]);
      if (!hw::sdLock(5000)) return false;
      if (!fs.remove(path)) ok = false;
      hw::sdUnlock();
    }
    if (k < 16) {
      if (!hw::sdLock(5000)) return false;
      if (!fs.rmdir(dir)) ok = false;
      hw::sdUnlock();
      break;
    }
  }
  return ok;
}

}  // namespace sessions
