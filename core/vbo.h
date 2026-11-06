// core/vbo.h - Racelogic VBO text format, for importing into RaceChrono.
// Latitude/longitude are written in minutes; longitude is positive to the WEST
// (Racelogic convention), so Japan comes out negative. Lines end with CR LF.
#pragma once

#include <cstdint>

namespace lt {

struct VboChannel {  // extra channel after the standard ones (CAN values etc.)
  const char* name;  // long name, may contain spaces; the column name uses '_' instead
  const char* unit;  // "" when none
};

struct VboDateTime {
  int year, month, day, hour, minute, second;
};

struct VboSample {
  int sats;
  uint32_t utcMsOfDay;  // UTC time of day [ms]
  int32_t latE7;        // [1e-7 deg], north positive
  int32_t lonE7;        // [1e-7 deg], east positive (converted on output)
  float speedKmh;
  float headingDeg;
  float heightM;
  const float* extra;   // one value per extra channel; may be null when there are none
};

// Both return the number of characters written (without the terminating NUL),
// or -1 when `cap` is too small.
int vboHeader(char* buf, int cap, const VboDateTime& created, const VboChannel* ch, int numCh,
              const char* comment);
int vboLine(char* buf, int cap, const VboSample& s, int numCh);

}  // namespace lt
