// core/store.h - XTSD space (プロジェクト計画 §3.4, 要件 8; 実装計画 5-6; UI設計 §7 データ):
// recording hours left, the low-space warning, and which sessions to delete when space is needed:
// downloaded sessions, oldest first, never the active one. Sessions that were not downloaded are
// never deleted; the plan only says how many of them stand in the way (for the warning).
#pragma once

#include <cstdint>

namespace lt {

constexpr int64_t kNominalBytesPerHour = 72000000;  // CSV 18 + VBO 9 + UBX 9 + IMU 36 MB per hour (§3.4)
constexpr float kLowFreeFraction = 0.20f;           // warn below 20 % free
constexpr int64_t kMinRateMeasureMs = 600000;       // a measured rate is used after 10 min of recording

struct StoreSession {
  const char* id;   // session id "YYYYMMDD_HHMMSS": sorts by time
  int64_t bytes;    // all of its files
  bool downloaded;  // a download marker exists (StorePlan::isDownloadMarker)
  bool active;      // being recorded now: never deleted
};

struct DeletePlan {
  int count;           // sessions to delete: order[0..count), oldest first (indices into the list)
  int64_t freedBytes;  // what they free
  bool enough;         // freedBytes >= the bytes asked for
  int blocking;        // when not enough: undownloaded sessions (oldest first) that would also have
                       // to go to free the rest. They are NOT in order[]: warn only.
  bool impossible;     // not enough even with every session except the active one
  int undownloaded;    // undownloaded sessions in the list, the active one not counted
};

class StorePlan {
 public:
  void setSpace(int64_t totalBytes, int64_t freeBytes);
  // What the open session has written so far and for how long (measured data rate)
  void setRecorded(int64_t bytes, int64_t recordMs);

  int64_t totalBytes() const { return total_; }
  int64_t freeBytes() const { return free_; }
  int64_t bytesPerHour() const;  // measured after kMinRateMeasureMs of recording, else nominal
  float hoursLeft() const;       // recording hours the free space holds
  float freeFraction() const;
  bool low() const;              // less than 20 % free
  // Bytes to free so that `hours` of recording fit (0: they fit already)
  int64_t shortfallFor(float hours) const;

  // Which sessions to delete to free needBytes. order: caller's array for the indices (orderCap
  // entries; a plan never needs more entries than sessions).
  DeletePlan plan(const StoreSession* s, int n, int64_t needBytes, int* order, int orderCap) const;

  // The session counts as downloaded when its main .csv, its .vbo or the report was downloaded
  // completely: marker ".dl_<id>_<cid>.csv", ".dl_<id>_<cid>.vbo" or ".dl_report.html" in its folder
  // (ARCH §5.1). cid null or "": "street".
  static bool isDownloadMarker(const char* fileName, const char* id, const char* cid);

 private:
  int64_t total_ = 0;
  int64_t free_ = 0;
  int64_t recBytes_ = 0;
  int64_t recMs_ = 0;
};

}  // namespace lt
