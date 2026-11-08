// timing.h - lap/sector timing and the ghost (best lap) for the active course, fed with every GNSS epoch.
// Wraps core/lap (LapTimer) and core/ghost (Ghost) for the firmware; called only from the GNSS task.
// Readers (UI, logger, BLE) take a copy with view() under the app lock.
#pragma once
#include <stdint.h>

#include "lt_core.h"

namespace timing {

constexpr int kMaxLaps = 64;  // laps kept for the lap page (oldest dropped)

struct Course {
  char id[32];  // lt::Course id (up to 31 characters)
  char name[64];
  lt::CourseDef def;
  int32_t centerLatE7, centerLonE7;
};

struct LapRow {
  int16_t lapNo;
  int32_t lapMs;
  int8_t numSplits;                   // 0 = sectors incomplete
  int32_t splitMs[lt::kMaxSectors + 1];
  int64_t startUtcMs, endUtcMs;
};

struct View {
  bool haveCourse;
  char courseId[32];
  char courseName[64];
  int numSectors;            // sector lines (splits = numSectors + 1)
  bool running;              // first S/F crossed
  int lapNo;                 // current lap (0 before the first crossing)
  int sector;                // 1-based sector of the current lap (0 before the first crossing)
  int64_t lapStartUs;        // GNSS time of the last S/F crossing
  int64_t lastLapMs;         // -1 none
  int64_t bestLapMs;         // -1 none
  int bestLapNo;
  bool deltaOk;              // ghost delta valid now
  float deltaS;              // now - best at the same place (negative = faster)
  bool ghostOk;              // best lap exists (ghost hand shown while deltaOk)
  int numLaps;               // rows in laps[] (newest last)
  LapRow laps[kMaxLaps];
  bool everStarted;          // a lap has started since the course became active (pre-run check)
};

// bufA/bufB: ghost trace buffers (PSRAM) of `cap` points each (≈ 2,300 points per 1:33 lap at 25 Hz)
void begin(lt::TracePoint* bufA, lt::TracePoint* bufB, int cap);
// Activates a course (nullptr = none). best: serialized reference lap (/best/<id>.bin, core/bestfile) or nullptr.
void setCourse(const Course* c, const uint8_t* best, size_t bestN);
// New session on the same course: laps restart at 1, the best lap (ghost) stays.
void newSession();
// The session closed: the running lap is dropped; the lap list stays for the lap page until newSession().
void endSession();
// Every GNSS epoch of a session on the course, with the row of the main log before it is written:
// feeds the lap timer and the ghost with lt::csvlog::fixOf(*row) (the values exactly as written, so
// lt::Replay of the file gives the same laps) and fills the row's lap, sector, lap_ms and ghost_delta_s.
// Returns lt::kEv* flags. Fills *done with the completed lap on kEvLap.
uint32_t onRow(lt::csvlog::MainRow* row, LapRow* done);
bool lapRunning();  // first S/F of the session crossed
// Ghost reference to save after kEvBest (serialized with core/bestfile). Returns bytes (0 = nothing).
size_t bestBlob(uint8_t* out, size_t cap);
void view(View* out);

}  // namespace timing
