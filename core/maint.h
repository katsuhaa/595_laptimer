// core/maint.h - maintenance items (/config/maint.json, ARCH §5.4, UI設計 §9) and calendar dates.
//
// maint.json:
//   {"version":1,"items":[{"id":"oil","name":"エンジンオイル","interval_km":5000,"interval_days":182,
//     "interval_track_h":0,"last_date":"2026-10-01","last_odo":12345,"last_track_h":12.5,"note":""}]}
//  - id ASCII [a-z0-9_] 1..23, unique; name UTF-8 (missing: the id); note UTF-8 (optional).
//  - interval_*: 0 (or missing) = that interval is not used.
//  - last_date "YYYY-MM-DD"; "" or null = not recorded. last_odo [km], last_track_h [h]: null or
//    missing = not recorded. An interval is evaluated only when its last_* value is recorded and the
//    current value (date, ODO, track hours) is known.
// Remaining: each evaluated interval gives "left" in its own unit and the fraction left
// (left / interval). The item shows the interval with the smallest fraction left.
// State: overdue when that fraction <= 0 (left <= 0: due now or past), warn when <= 20 %, else ok;
// unknown when no interval can be evaluated. Order of urgency: overdue (most overdue first), warn,
// ok (least left first), unknown; equal ones in file order.
#pragma once

#include <cstdint>

#include "json.h"

namespace lt {

// ---- calendar dates as day numbers (days since 1970-01-01, proleptic Gregorian) ----------------
constexpr int32_t kNoDate = -2147483647 - 1;
int32_t daysFromCivil(int y, int m, int d);
void civilFromDays(int32_t day, int* y, int* m, int* d);
bool parseDate(const char* s, int n, int32_t* day);  // "YYYY-MM-DD", a real calendar date, 1900..9999
int formatDate(char* out, int cap, int32_t day);     // "YYYY-MM-DD"; 0 when cap < 11 or kNoDate
int32_t dayOfUtcMs(int64_t utcMs, int tzMin);         // local calendar day of a UTC time

// ---- maintenance -------------------------------------------------------------------------------
constexpr int kMaintMaxItems = 16;
constexpr float kMaintWarnFraction = 0.20f;  // warn at 20 % left or less

enum class MaintState : uint8_t { Overdue, Warn, Ok, Unknown };  // most urgent first
enum class MaintUnit : uint8_t { None, Km, Days, TrackH };

struct MaintItem {
  char id[24];
  char name[64];  // UTF-8
  int32_t intervalKm;  // 0 = not used
  int32_t intervalDays;
  float intervalTrackH;
  int32_t lastDay;  // kNoDate: not recorded
  bool hasOdo;
  float lastOdoKm;
  bool hasTrackH;
  float lastTrackH;
  char note[128];  // UTF-8
};

struct MaintNow {
  int32_t today = kNoDate;  // local calendar day (dayOfUtcMs), kNoDate when unknown
  bool odoOk = false;
  float odoKm = 0.0f;       // ODO now (DriveState::odometerKm)
  bool trackOk = false;
  float trackH = 0.0f;      // track hours now (DriveState::trackHours)
};

struct MaintStatus {
  int index;          // the item: Maint::item(index)
  MaintState state;
  MaintUnit unit;     // the interval with the smallest fraction left (None: unknown)
  float remaining;    // left in that unit [km | days | h]; <= 0: due
  float fraction;     // remaining / interval (1 = just done)
  bool hasKm, hasDays, hasTrackH;  // which intervals were evaluated
  float remKm, remDays, remTrackH;
  float fracKm, fracDays, fracTrackH;
};

class Maint {
 public:
  // Replaces the content; on failure the list is empty. Codes: JSON syntax (json.h) or bad-version,
  // missing-version, missing-items, too-many-items, bad-id, missing-id, duplicate-id,
  // name-too-long, note-too-long, bad-interval, bad-date, bad-odo, bad-track-h.
  bool load(json::Source& src, json::Error* err);
  bool load(const char* text, int len, json::Error* err);
  static bool check(json::Source& src, json::Error* err);  // the same checks, nothing kept

  void clear() { n_ = 0; }
  int count() const { return n_; }
  const MaintItem& item(int i) const { return items_[i]; }
  int find(const char* id) const;  // -1: none

  MaintStatus status(int i, const MaintNow& now) const;
  // Every item, most urgent first. Returns the number written (at most cap).
  int evaluate(const MaintNow& now, MaintStatus* out, int cap) const;

  // "実施済み": last_date = now.today, last_odo = now.odoKm, last_track_h = now.trackH
  // (a value that is not known now becomes "not recorded"). false: no such id, or the date is unknown.
  bool markDone(const char* id, const MaintNow& now);

  int toJson(char* out, int cap) const;  // maint.json text (unknown keys of the loaded file are not kept)

 private:
  MaintItem items_[kMaintMaxItems];
  int n_ = 0;
};

}  // namespace lt
