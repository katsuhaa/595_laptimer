// core/replay.h - the main log CSV (ARCH §5.2) read back: the replay mode (/replay/*.csv, 計画 要件 14,
// 実装計画 3-1) and the host tools. The header row decides which column is which (other columns and
// another order are fine); an empty field is "not available". Each data row gives the whole
// csvlog::MainRow (live values and the lap/sector/lap_ms the device showed) and the lap timer input:
// the same lt::Fix as csvlog::fixOf() made live, so the lap timer gives the same laps to the µs.
#pragma once

#include <cstdint>

#include "csvlog.h"
#include "lap.h"

namespace lt {

struct ReplayRow {
  csvlog::MainRow row;  // values of the line (the *Ok flags tell which fields were there)
  Fix fix;              // lap timer input (= csvlog::fixOf(row) of the live run)
  int badFields;        // fields that were not empty but could not be read (left out)
};

class Replay {
 public:
  static constexpr int kMaxColumns = 64;

  // The header row (a UTF-8 BOM, CR and LF are allowed). false: utc_ms, lat, lon, speed_kmh or fix
  // is missing, or more than kMaxColumns columns.
  bool header(const char* line, int len = -1);
  bool ready() const { return numCols_ > 0; }
  int numColumns() const { return numCols_; }
  int column(const char* name) const;  // index of a §5.2 column, -1 when the file does not have it

  // One data line. false (and why, may be null): "no-header", "empty", "header" (the header row
  // again), "columns" (not as many fields as the header: e.g. a line cut by a power loss).
  bool parse(const char* line, int len, ReplayRow* out, const char** why = nullptr) const;

  // The text of field `col` of a line (no copy). false: the line has fewer fields.
  static bool field(const char* line, int len, int col, const char** s, int* n);

 private:
  int8_t at_[kMaxColumns];  // column index -> known field (-1: not used)
  int known_[24];           // known field -> column index (-1: not in the file)
  int numCols_ = 0;
};

}  // namespace lt
