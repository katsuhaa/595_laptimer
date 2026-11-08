// timing.cpp - see timing.h
#include "timing.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace timing {
namespace {

SemaphoreHandle_t g_mu = nullptr;
lt::LapTimer g_lap;
lt::Ghost g_ghost;
lt::Plane g_plane;
Course g_course = {};
bool g_have = false;
View g_view = {};
int64_t g_lapStartUtcMs = 0;
lt::TracePoint* g_tmp = nullptr;  // reading /best/<id>.bin (same capacity as the ghost buffers)
int g_tmpCap = 0;

struct Lock {
  Lock() { xSemaphoreTake(g_mu, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(g_mu); }
};

void pushLap(const LapRow& r) {
  if (g_view.numLaps == kMaxLaps) {
    memmove(&g_view.laps[0], &g_view.laps[1], sizeof(LapRow) * (kMaxLaps - 1));
    g_view.numLaps--;
  }
  g_view.laps[g_view.numLaps++] = r;
}

}  // namespace

void begin(lt::TracePoint* bufA, lt::TracePoint* bufB, int cap) {
  g_mu = xSemaphoreCreateMutex();
  g_ghost.begin(bufA, bufB, cap);
  g_tmp = static_cast<lt::TracePoint*>(heap_caps_malloc(sizeof(lt::TracePoint) * static_cast<size_t>(cap),
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  g_tmpCap = g_tmp ? cap : 0;
}

void endSession() {
  Lock l;
  if (!g_have) return;
  g_lap.reset();
  g_view.running = false;
  g_view.sector = 0;
  g_view.deltaOk = false;
}

void setCourse(const Course* c, const uint8_t* best, size_t bestN) {
  Lock l;
  memset(&g_view, 0, sizeof g_view);  // (no 4 KB temporary on the caller's stack)
  g_ghost.clearBest();
  g_have = c != nullptr;
  if (!g_have) return;
  g_course = *c;
  g_plane.init(c->centerLatE7, c->centerLonE7);
  g_lap.begin(g_plane, c->def);
  g_view.haveCourse = true;
  strncpy(g_view.courseId, c->id, sizeof g_view.courseId - 1);
  strncpy(g_view.courseName, c->name, sizeof g_view.courseName - 1);
  g_view.numSectors = c->def.numSectors;
  g_view.lastLapMs = -1;
  g_view.bestLapMs = -1;
  if (best && bestN) {
    // The reference lap is stored as lat/lon (core/bestfile) and projected on this course's plane
    lt::bestfile::Info info = {};
    const char* why = "";
    const int n = lt::bestfile::readTrace(best, static_cast<int>(bestN), g_plane, g_tmp, g_tmpCap, &info, &why);
    if (n >= 2 && strcmp(info.courseId, c->id) == 0 && g_ghost.setBest(g_tmp, n)) {
      g_view.ghostOk = true;
      g_view.bestLapMs = info.lapMs;
    }
  }
}

void newSession() {
  Lock l;
  if (!g_have) return;
  g_lap.reset();
  const int64_t best = g_view.bestLapMs;
  const bool ghost = g_view.ghostOk;
  memset(&g_view, 0, sizeof g_view);
  g_view.haveCourse = true;
  strncpy(g_view.courseId, g_course.id, sizeof g_view.courseId - 1);
  strncpy(g_view.courseName, g_course.name, sizeof g_view.courseName - 1);
  g_view.numSectors = g_course.def.numSectors;
  g_view.lastLapMs = -1;
  g_view.bestLapMs = best;
  g_view.ghostOk = ghost;
}

uint32_t onRow(lt::csvlog::MainRow* row, LapRow* done) {
  Lock l;
  row->lap = 0;
  row->sector = 0;
  row->lapMsOk = false;
  row->lapMs = 0;
  row->ghostOk = false;
  row->ghostDeltaS = 0.0f;
  if (!g_have) return 0;
  const lt::Fix f = lt::csvlog::fixOf(*row);  // f.tUs = utc_ms * 1000
  uint32_t ev = g_lap.feed(f);
  const lt::Vec2 pos = g_plane.toLocal(f.latE7, f.lonE7);
  if (ev & (lt::kEvLap | lt::kEvLapStart)) {
    const int64_t t = g_lap.sfCrossUs();
    const lt::Vec2 x = g_lap.sfCrossPos();
    const int64_t crossUtcMs = t / 1000;  // the lap timer runs on UTC (fixOf)
    if (ev & lt::kEvLap) {
      const lt::LapResult& r = g_lap.lastLap();
      LapRow lr = {};
      lr.lapNo = static_cast<int16_t>(r.lapNo);
      lr.lapMs = static_cast<int32_t>(r.lapUs / 1000);
      lr.numSplits = static_cast<int8_t>(r.numSplits);
      for (int i = 0; i < r.numSplits && i <= lt::kMaxSectors; ++i) lr.splitMs[i] = static_cast<int32_t>(r.splitUs[i] / 1000);
      lr.startUtcMs = g_lapStartUtcMs;
      lr.endUtcMs = crossUtcMs;
      // Best = faster than the reference best (the stored /best file or an earlier session): the lap
      // timer's own kEvBest restarts with every session, so its first lap would always be "best"
      const bool best = g_view.bestLapMs < 0 || lr.lapMs < g_view.bestLapMs;
      ev = best ? (ev | lt::kEvBest) : (ev & ~lt::kEvBest);
      g_ghost.endLap(t, x, best);  // a new best replaces the ghost reference; else the previous one stays
      pushLap(lr);
      if (done) *done = lr;
      g_view.lastLapMs = lr.lapMs;
      if (best) {
        g_view.bestLapMs = lr.lapMs;
        g_view.bestLapNo = lr.lapNo;
        g_view.ghostOk = g_ghost.hasBest();
      }
    }
    g_ghost.startLap(t, x);
    g_lapStartUtcMs = crossUtcMs;
    g_view.everStarted = true;
  } else if (g_lap.timing()) {
    g_ghost.addSample(f.tUs, pos);
  }
  g_view.running = g_lap.timing();
  g_view.lapNo = g_lap.currentLapNo();
  g_view.sector = g_lap.timing() ? g_lap.sectorsDone() + 1 : 0;
  g_view.lapStartUs = g_lap.lapStartUs();
  float d = 0.0f;
  g_view.deltaOk = g_lap.timing() && g_ghost.delta(f.tUs, pos, &d);
  g_view.deltaS = d;
  lt::csvlog::setLapFields(row, g_lap, f.tUs);
  row->ghostOk = g_view.deltaOk;
  row->ghostDeltaS = d;
  return ev;
}

bool lapRunning() {
  Lock l;
  return g_have && g_view.running;
}

size_t bestBlob(uint8_t* out, size_t cap) {
  Lock l;
  if (!g_have || !g_ghost.hasBest()) return 0;
  const int n = lt::bestfile::writeTrace(out, static_cast<int>(cap), g_course.id, static_cast<uint32_t>(g_view.bestLapMs),
                                         g_plane, g_ghost.bestTrace(), g_ghost.bestCount());
  return n > 0 ? static_cast<size_t>(n) : 0;
}

void view(View* out) {
  Lock l;
  *out = g_view;
}

}  // namespace timing
