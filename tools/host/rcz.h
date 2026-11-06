// tools/host/rcz.h - reads a RaceChrono session export (.rcz) after it has been unzipped.
// The layout was found by inspecting RaceChrono Pro (iOS) exports of 2026-10/11:
//   session.json          track name, laps (start/finish timestamps, epoch ms)
//   sessionfragment.json  devices (the GPS device: primaryGpsDeviceIndex or the first type 1)
//   channel_1_<dev>_0_<id>_<t>  little-endian values, one per GPS epoch:
//     id 1   int64  timestamp [ms since 1970-01-01 UTC]
//     id 3   int32 latitude + int32 longitude [1/6,000,000 deg]
//     id 4   int32  speed [mm/s]
//     id 5   int32  altitude [mm]
//     id 6   int32  bearing [1/1000 deg]
//     id 30002 int32 satellites (when present)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rcz {

struct Lap {
  int number = 0;
  int64_t startMs = 0;
  int64_t finishMs = 0;
  bool finished = false;  // the last lap of a session has no finish
  bool invalid = false;
};

struct Session {
  std::string name;  // directory name
  std::string trackName;
  int gpsDevice = 0;
  std::vector<Lap> laps;
  std::vector<int64_t> tMs;
  std::vector<int32_t> latE7;
  std::vector<int32_t> lonE7;
  std::vector<float> speedMps;
  std::vector<float> bearingDeg;
  std::vector<float> altM;
  std::vector<int> sats;  // empty when the session has no satellite channel
};

bool load(const std::string& dir, Session* s, std::string* err);

}  // namespace rcz
