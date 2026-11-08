#include "store.h"

#include <cstring>

namespace lt {

namespace {

// a before b: older session (ids sort by time); equal ids by position in the list
bool before(const StoreSession* s, int a, int b) {
  const int c = std::strcmp(s[a].id ? s[a].id : "", s[b].id ? s[b].id : "");
  return c != 0 ? c < 0 : a < b;
}

// The oldest deletable-kind session after `after` (-1: from the start), active ones never
int nextOldest(const StoreSession* s, int n, int after, bool downloaded) {
  int best = -1;
  for (int i = 0; i < n; ++i) {
    if (s[i].active || s[i].downloaded != downloaded) continue;
    if (after >= 0 && !before(s, after, i)) continue;
    if (best < 0 || before(s, i, best)) best = i;
  }
  return best;
}

}  // namespace

void StorePlan::setSpace(int64_t totalBytes, int64_t freeBytes) {
  total_ = totalBytes > 0 ? totalBytes : 0;
  free_ = freeBytes > 0 ? freeBytes : 0;
  if (total_ > 0 && free_ > total_) free_ = total_;
}

void StorePlan::setRecorded(int64_t bytes, int64_t recordMs) {
  recBytes_ = bytes > 0 ? bytes : 0;
  recMs_ = recordMs > 0 ? recordMs : 0;
}

int64_t StorePlan::bytesPerHour() const {
  if (recMs_ >= kMinRateMeasureMs && recBytes_ > 0) {
    const int64_t r = recBytes_ * 3600000 / recMs_;
    if (r > 0) return r;
  }
  return kNominalBytesPerHour;
}

float StorePlan::hoursLeft() const {
  return static_cast<float>(free_) / static_cast<float>(bytesPerHour());
}

float StorePlan::freeFraction() const {
  return total_ > 0 ? static_cast<float>(free_) / static_cast<float>(total_) : 0.0f;
}

bool StorePlan::low() const { return total_ > 0 && free_ * 100 < total_ * 20; }

int64_t StorePlan::shortfallFor(float hours) const {
  if (!(hours > 0.0f)) return 0;
  const int64_t need = static_cast<int64_t>(hours * static_cast<float>(bytesPerHour()));
  return need > free_ ? need - free_ : 0;
}

DeletePlan StorePlan::plan(const StoreSession* s, int n, int64_t needBytes, int* order, int orderCap) const {
  DeletePlan p{};
  for (int i = 0; i < n; ++i) {
    if (!s[i].active && !s[i].downloaded) ++p.undownloaded;
  }
  if (needBytes <= 0) {
    p.enough = true;
    return p;
  }
  int at = -1;
  while (p.freedBytes < needBytes && p.count < orderCap) {
    const int i = nextOldest(s, n, at, true);
    if (i < 0) break;
    at = i;
    if (s[i].bytes <= 0) continue;  // frees nothing
    order[p.count++] = i;
    p.freedBytes += s[i].bytes;
  }
  p.enough = p.freedBytes >= needBytes;
  if (!p.enough) {
    int64_t rest = needBytes - p.freedBytes;
    at = -1;
    while (rest > 0) {
      const int i = nextOldest(s, n, at, false);
      if (i < 0) break;
      at = i;
      if (s[i].bytes <= 0) continue;
      ++p.blocking;
      rest -= s[i].bytes;
    }
    p.impossible = rest > 0;
  }
  return p;
}

bool StorePlan::isDownloadMarker(const char* fileName, const char* id, const char* cid) {
  if (!fileName || !id || !id[0] || std::strncmp(fileName, ".dl_", 4) != 0) return false;
  const char* f = fileName + 4;
  if (std::strcmp(f, "report.html") == 0) return true;
  const char* c = cid && cid[0] ? cid : "street";
  const size_t il = std::strlen(id);
  const size_t cl = std::strlen(c);
  if (std::strncmp(f, id, il) != 0 || f[il] != '_' || std::strncmp(f + il + 1, c, cl) != 0) return false;
  const char* ext = f + il + 1 + cl;
  return std::strcmp(ext, ".csv") == 0 || std::strcmp(ext, ".vbo") == 0;
}

}  // namespace lt
