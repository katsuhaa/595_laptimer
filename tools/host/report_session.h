// tools/host/report_session.h - a session in the ARCH 5 layout made from the owner's RaceChrono data,
// for the host `report` command and the Web page tests (mock server).
//   RaceChrono VBO export: GPS 10 Hz + the CAN channels RaceChrono recorded (rpm, accelerator, ...)
//   RaceChrono .rcz (unzipped): GPS only (25/20/10 Hz); G and yaw are computed from the GPS track
// Laps, sectors, lap_ms and ghost_delta_s come from core/lap and core/ghost with RaceChrono's track
// definition ([laptiming] of a VBO export), S/F 50 m and split lines 30 m wide, as tools/host/laps.cpp.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "core/lap.h"
#include "tools/host/vboread.h"

namespace rs {

constexpr double kNan = NAN;

// One main-log row (ARCH 5.2). NaN / -1 / 0 = empty field as noted.
struct Row {
  int64_t utcMs = 0;
  double lat = kNan, lon = kNan, altM = kNan, speedKmh = kNan, headingDeg = kNan, haccM = kNan;
  int sats = -1;  // -1 empty
  int fix = -1;
  double lonG = kNan, latG = kNan, vertG = kNan, yawDps = kNan;
  double rpm = kNan, throttlePct = kNan, speedCanKmh = kNan, waterC = kNan, fuelPct = kNan, boostBar = kNan;
  char gear = 0;  // 0 empty
  int lap = 0;
  int sector = 0;
  int64_t lapMs = -1;  // -1 empty
  double ghostDeltaS = kNan;
};

struct LapRow {
  int lap = 0;
  int64_t startUtcMs = 0;
  int64_t endUtcMs = 0;
  int64_t lapMs = 0;
  std::vector<int64_t> sectorsMs;  // empty when a sector line was missed
};

struct Session {
  std::string id;  // local start time YYYYMMDD_HHMMSS
  int tzMin = 540;
  std::string courseId = "course";
  std::string courseName;
  std::string source;  // input file
  double rateHz = 0.0;
  std::vector<Row> rows;
  std::vector<LapRow> laps;
  std::vector<std::string> notes;  // how the data was made (meta.json "notes", shown in the report)
  // RaceChrono's own laps (.rcz only): start/finish [utc ms], for the comparison
  std::vector<std::pair<int64_t, int64_t>> raceChronoLaps;
  lt::CourseDef course{};  // the lines the laps were timed with
  int32_t planeLatE7 = 0;  // origin of the local plane (the S/F point)
  int32_t planeLonE7 = 0;
  std::vector<std::string> lineNames;  // "Start/Finish", "Split 1", ...
};

struct Options {
  const vboread::File* course = nullptr;  // RaceChrono track definition (for .rcz input)
  std::string coursesJson;                // optional courses.json: course id / name by S/F position
  int tzMin = 540;                        // JST
};

// Reads a RaceChrono VBO export (with [laptiming]) into a session.
bool fromVboExport(const std::string& path, const Options& opt, Session* s, std::string* err);
// Reads an unzipped RaceChrono .rcz directory (course from opt.course).
bool fromRcz(const std::string& dir, const Options& opt, Session* s, std::string* err);

// The lap timer on the session's own fixes (an independent run, for the checks).
std::vector<lt::LapResult> lapTimer(const Session& s, const lt::CourseDef& c);

// Writes <root>/sessions/<id>/ : meta.json, <id>_<cid>.csv, <id>_<cid>_laps.csv, <id>_<cid>.vbo.
bool writeFolder(const Session& s, const std::string& root, std::string* dir, std::string* err);

// ARCH 5.2 / 5.1 text
std::string mainHeader();
std::string mainLine(const Row& r);
std::string lapsHeader();
std::string lapsLine(const LapRow& l);
std::string metaJson(const Session& s, const std::string& csv, const std::string& vbo, const std::string& laps);

// "1:23.456"
std::string lapText(int64_t ms);

}  // namespace rs
