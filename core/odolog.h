// core/odolog.h - odometer log (走行記録): port of the round gauge's `odo` (katsuhaa/abarth595-round-gauge
// 2.15.0-2.15.3, meter_display/app.cpp namespace odo, config.h ODO_*; spec odo_log_spec.md). Same record
// format, write rule, day table and periods, so the behaviour on both devices is the same.
//
// For every local calendar day (JST) the distance (difference of the B-CAN odometer, km) and the over-rpm
// time (▲: time at or above 「過負荷 回転数」, s) are appended to a flash area as 32-byte records. Each record holds
// that day's running total (the newest record of a day is the day's value). A record is written when
// all of these hold: slow (<= ODO_LOW_KMH, standing still included) or in reverse; the time is known;
// the odometer or the over-rpm time grew since the last record; ODO_MIN_INTERVAL_MS since the last write.
// After midnight the closing record of the previous day is written the next time writing is allowed.
// The area is used as a ring of ODO_SECTOR_BYTES sectors: a sector is erased before its first record.
// At begin() every record is read back into a day table (caller's buffer, e.g. PSRAM).
//
// Differences from the round gauge:
//  - the over-rpm time is Trip::aboveMs (▲, the round gauge's trip::aboveMs: at or above 「過負荷 回転数」)
//  - storage and the day table come from the caller (no heap in core/)
#pragma once

#include <cstddef>
#include <cstdint>

namespace lt {

// round gauge config.h ODO_*
constexpr float ODO_LOW_KMH = 10.0f;               // owner 11/2
constexpr float ODO_STILL_KMH = 0.5f;              // slower: the record's reason is "standing still"
constexpr uint32_t ODO_MIN_INTERVAL_MS = 60000;    // owner 11/2
constexpr int ODO_REC_BYTES = 32;
constexpr int ODO_SECTOR_BYTES = 4096;
constexpr int ODO_TABLE_DAYS = 4096;               // days in the day table (12 bytes a day in the round gauge)
constexpr int ODO_WEEK_START = 0;                  // first day of a week (0 = Sunday, owner 11/8)
constexpr uint32_t ODO_AREA_BYTES = 0xE0000;       // 896 KB = 28,672 records, as the round gauge's spiffs
constexpr int kOdoTzSec = 9 * 3600;                // JST (round gauge CLOCK_TZ_SEC)

// Flash area. Erase works on ODO_SECTOR_BYTES boundaries; written bits only go 1 -> 0. true = done.
class OdoStorage {
 public:
  virtual ~OdoStorage() = default;
  virtual uint32_t size() = 0;  // 0 = no area
  virtual bool read(uint32_t off, void* data, size_t n) = 0;
  virtual bool write(uint32_t off, const void* data, size_t n) = 0;
  virtual bool erase(uint32_t off, size_t n) = 0;
};

class OdoLog {
 public:
  struct DayEnt {  // one slot of the day table: day d is at d % ODO_TABLE_DAYS
    int32_t day1;    // d + 1 (0 = empty)
    uint32_t seq;    // sequence number of the record the values come from (the newest)
    uint32_t start;  // odometer at the start of the day
    uint32_t odo;    // last odometer of the day
    uint32_t aboveS;
  };
  static constexpr size_t kTableBytes = sizeof(DayEnt) * ODO_TABLE_DAYS;

  struct Input {
    bool odoOk;      // odometer received within ODO_STALE_MS
    bool odoHave;    // odometer received since power-up
    uint32_t odoKm;
    bool speedOk;
    float speedKmh;
    bool reverse;
    bool timeOk;     // time known (GNSS, or the RTC set from it)
    uint32_t utc;    // UTC seconds
    uint32_t aboveMs;  // Trip::aboveMs() (▲): what it grew by is added to the day (a drop is a reset: not added)
  };
  enum Reason : uint8_t { R_STILL = 1, R_LOW = 2, R_REVERSE = 3, R_CLOSE = 4 };
  struct Status {
    bool storageOk;     // the area exists and was read
    uint32_t records;   // valid records in the area
    uint32_t capacity;  // records that fit
    uint32_t writes;    // written since begin()
    uint32_t failures;  // writes that failed (written and read back different)
    bool timeKnown;     // today is known
    int32_t today;      // local day number (when timeKnown)
    uint32_t todayKm;   // today's distance (also what is not written yet); before the time: since power-up
    uint32_t todayAboveS;
    uint32_t lastUtc;   // time of the last record (0 = none)
    uint8_t lastReason;
    uint32_t lastKm;    // that record's distance of its day
    uint32_t lastAboveS;
    uint32_t lastOdo;
    bool curOk;         // the odometer was received
    uint32_t cur;       // odometer now
  };
  // Periods (round gauge 2.15.2): 1 year, 3 months (Jan-Mar, Apr-Jun, Jul-Sep, Oct-Dec), 1 month,
  // 1 week (ODO_WEEK_START first), 1 day
  enum Level { LV_YEAR = 0, LV_QUARTER = 1, LV_MONTH = 2, LV_WEEK = 3, LV_DAY = 4, LV_COUNT = 5 };
  struct Period {
    int32_t first, last;  // first and last day (both included)
    bool current;         // contains today
    bool odoKnown;        // the end odometer is known (a record in or before the period; today: now)
    uint32_t odoEnd;      // odometer at the end of the period (now for the current one)
    uint32_t odoStart;    // odometer at its start (end of the day before)
    uint32_t km;          // odoEnd - odoStart
    uint32_t aboveS;      // over-rpm time of the period
    bool hasRecord;       // some day of it has a record (today's values included)
    bool canBack;         // an earlier period exists (the oldest record is before it)
    bool canFwd;          // a later period exists (before today)
  };

  // table: kTableBytes (nullptr = no day table: nothing is shown, records are still written)
  void begin(OdoStorage* storage, void* table);
  void update(const Input& in, uint32_t nowMs);  // call often; writes one record when the rule says so
  void status(Status* out) const;
  bool wroteSince(uint32_t* writes) const;  // writes grew since *writes: true and *writes updated

  bool day(int32_t d, uint32_t* km, uint32_t* aboveS) const;  // a day's values; false (0) = no record
  int32_t earliest() const;  // day of the oldest record (today without records; 0 when unknown)
  bool today(int32_t* d) const;
  bool periodAt(int level, int32_t anchor, Period* out) const;  // anchor after today = today. false: no date
  static void bounds(int level, int32_t d, int32_t* first, int32_t* last);
  static int32_t prevAnchor(int level, int32_t anchor);    // last day of the previous period
  int32_t nextAnchor(int level, int32_t anchor) const;     // last day of the next one (not after today)

  // local day number <-> date (1970-01-01 = 0, Thursday; wday 0 = Sunday)
  static void civil(int32_t d, int* y, int* m, int* dd, int* wday);
  static int32_t daysFrom(int y, int m, int d);
  static int32_t dayOfUtc(uint32_t utc) { return static_cast<int32_t>((static_cast<uint64_t>(utc) + kOdoTzSec) / 86400u); }

 private:
  struct Rec {
    uint8_t reason;
    uint32_t seq, utc;
    int32_t day;
    uint32_t odo, start, aboveS;
  };
  static void encode(const Rec& r, uint8_t* o);
  static bool decode(const uint8_t* in, Rec* r);
  static uint32_t recKm(const Rec& r) { return r.odo >= r.start ? r.odo - r.start : 0; }
  DayEnt* slotOf(int32_t d) const;
  void tabPut(int32_t d, uint32_t seq, uint32_t start, uint32_t odo, uint32_t aboveS);
  uint32_t countValid(uint32_t off);
  bool eraseAt(uint32_t off);
  bool writeRec(Rec r, uint32_t nowMs);
  bool dayRec(int32_t d, uint32_t* start, uint32_t* odo, uint32_t* aboveS) const;
  bool odoAt(int32_t d, uint32_t* odo) const;
  void resetState();

  OdoStorage* st_ = nullptr;
  DayEnt* tab_ = nullptr;
  uint8_t buf_[ODO_SECTOR_BYTES];  // reading at begin(), counting before an erase
  bool storageOk_ = false;
  uint32_t slots_ = 0, pos_ = 0, seq_ = 0;
  uint32_t records_ = 0, writes_ = 0, failures_ = 0;
  int32_t earliest_ = 0;
  bool haveEarliest_ = false;
  bool haveLatest_ = false;  // newest record read at begin() (today's continuation)
  Rec latest_{};
  bool haveLast_ = false;    // last record written (status)
  Rec last_{};
  bool dayKnown_ = false;
  int32_t curDay_ = 0;
  bool startSet_ = false;
  uint32_t start_ = 0;
  uint32_t aboveDayMs_ = 0;  // the day's over-rpm time [ms] (before the time is known: since power-up)
  bool aboveInit_ = false;
  uint32_t lastAbove_ = 0;
  uint32_t cur_ = 0;
  bool wrote_ = false;
  uint32_t lastWriteMs_ = 0;
  bool closePending_ = false;
  Rec close_{};
  bool lwHave_ = false;  // values of the last record written (did they grow)
  int32_t lwDay_ = 0;
  uint32_t lwOdo_ = 0, lwAbove_ = 0;
};

// Which period the screen shows (round gauge ui.cpp g_odoLevel / g_odoAnchor, 2.15.0-2.15.3): one level and
// one day in the period (anchor). Opening shows this year. Back / forward stop at the oldest record's
// period and at the current one. A finer level shows the last period of the one shown that is not after
// today; a coarser level shows the period that holds the anchor (the one the finer view came from).
class OdoNav {
 public:
  int level() const { return level_; }
  int32_t anchor() const { return anchor_; }
  void open(const OdoLog& log);              // 1 year, today
  bool prev(const OdoLog& log);              // false: nothing earlier (or no date)
  bool next(const OdoLog& log);              // false: already the current period
  bool setLevel(int level, const OdoLog& log);  // false: no date yet, or level out of range
  bool canFiner(const OdoLog& log) const;    // not 1 day, and the date is known

 private:
  int level_ = OdoLog::LV_YEAR;
  int32_t anchor_ = 0;
};

}  // namespace lt
