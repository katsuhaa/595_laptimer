// logger.h - session files on the XTSD (ARCH §5.1): one writer task, lock-free producers.
// Producers (GNSS / IMU / link tasks) hand over already formatted text or raw bytes; nothing blocks them:
// if a stream buffer is full the data is dropped and counted (shown in the self-test / data page).
// The logger task writes in large chunks under the storage mutex and syncs every second (要件 8: キー OFF で壊れない).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace logger {

enum Stream : uint8_t { S_MAIN = 0, S_VBO, S_UBX, S_IMU, S_CAN, S_COUNT };

struct OpenInfo {
  char id[16];          // YYYYMMDD_HHMMSS (local)
  char cid[32];         // course id or "street" (lt::Course id: up to 31 characters)
  bool canRaw;          // also write <id>_<cid>_can.csv
  const char* mainHeader;  // header rows written first (pointers must stay valid until the session is open)
  const char* vboHeader;
  const char* imuHeader;
  const char* canHeader;
  const char* lapsHeader;
  const char* metaJson;    // first meta.json
};

struct Stats {
  bool open;
  char id[16];
  uint32_t bytes[S_COUNT];     // written this session
  uint32_t drops[S_COUNT];     // bytes dropped because a buffer was full (session total)
  uint32_t maxWriteMs;         // longest single write under the lock
  uint32_t syncs;              // fsync rounds
  uint32_t errors;             // failed writes/opens
  char lastError[48];
};

void begin();                        // allocates the stream buffers (PSRAM); call before the tasks start
bool requestOpen(const OpenInfo& o); // queued for the logger task; false if a session is already open/opening
void requestClose(const char* finalMetaJson);
bool isOpen();

// Producers. Each stream has exactly one producer task (stream buffers are single-writer).
void write(Stream s, const void* data, size_t n);
// Control records from any task (copied):
void lapLine(const char* line);             // appended to <id>_<cid>_laps.csv
void meta(const char* json);                // replaces meta.json
void bestLap(const char* cid, const uint8_t* data, size_t n);  // replaces /best/<cid>.bin (atomic via temp file)

void stats(Stats* out);
void task(void*);

}  // namespace logger
