// sessions.h - the list of recorded sessions on the XTSD (/sessions/<id>/meta.json + .dl_* markers, ARCH §5.1)
// for the data page, the capacity management (5-6) and the web hooks. Cached; rescan after a change.
#pragma once
#include <stdint.h>

namespace sessions {

constexpr int kMax = 256;

struct Info {
  char id[16];
  char cid[32];
  char name[64];        // course name ("" = street)
  int64_t startUtcMs;
  int64_t endUtcMs;
  int16_t laps;
  int32_t bestMs;       // -1 none
  bool closed;          // meta.json says closed (false = still open or the power was cut)
  bool downloaded;      // .csv, .vbo or the report was downloaded completely
  uint64_t bytes;       // all files of the session
};

void begin();                  // allocates the cache (PSRAM)
int rescan();                  // reads the card (takes the storage lock); returns the count
int count();
bool get(int i, Info* out);    // 0 = newest
int undownloaded();
uint64_t totalBytes();
bool removeSession(const char* id);  // deletes the folder (storage lock inside); call rescan() afterwards

}  // namespace sessions
