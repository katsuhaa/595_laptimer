// tools/host/vboread.h - reads the parts of a VBO file the host tools need
// (RaceChrono's exports and our own files).
// RaceChrono writes its track definition into [laptiming]; one line per timing line:
//   Start   -8405.283480 +2123.694540 -8405.257911 +2123.659851 ¬ Start/Finish
//   Split   lonA latA lonB latB ¬ name      (minutes, longitude positive to the WEST)
// Checked on its 2026-10-31 export against the laps of three sessions: point A lies on the
// line, and B lies 50-75 m behind A, so B -> A is the direction of travel.
#pragma once

#include <string>
#include <vector>

namespace vboread {

struct Gate {
  std::string kind;  // "Start" or "Split"
  std::string name;
  double latA = 0.0, lonA = 0.0;  // on the line [deg], east positive
  double latB = 0.0, lonB = 0.0;  // behind it
};

struct File {
  std::string path;
  std::string created;              // the first line
  std::string sessionName;          // [session data] "name ..."
  std::vector<std::string> header;  // [header] lines: channel name, unit after a space
  std::vector<Gate> gates;          // [laptiming]
};

bool read(const std::string& path, File* f, std::string* err);

}  // namespace vboread
