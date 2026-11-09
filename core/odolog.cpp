#include "odolog.h"

#include <cstring>

namespace lt {

namespace {

constexpr uint16_t kMagic = 0x4F44;  // "OD"
constexpr uint8_t kVer = 1;
constexpr int kSlotsPerSector = ODO_SECTOR_BYTES / ODO_REC_BYTES;

void put16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t crc16(const uint8_t* p, size_t n) {  // CRC-16/CCITT-FALSE
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    c = static_cast<uint16_t>(c ^ (static_cast<uint16_t>(p[i]) << 8));
    for (int b = 0; b < 8; b++)
      c = (c & 0x8000) ? static_cast<uint16_t>((c << 1) ^ 0x1021) : static_cast<uint16_t>(c << 1);
  }
  return c;
}
bool blank(const uint8_t* p) {
  for (int i = 0; i < ODO_REC_BYTES; i++)
    if (p[i] != 0xFF) return false;
  return true;
}

}  // namespace

// 32 bytes (spec chapter 5): magic 2, version 1, reason 1, sequence 4, time 4, day 2, odometer 4,
// start odometer 4, over-rpm time 4, spare 4 (0xFF), CRC 2. Same bytes as the round gauge.
void OdoLog::encode(const Rec& r, uint8_t* o) {
  std::memset(o, 0xFF, ODO_REC_BYTES);
  put16(o, kMagic);
  o[2] = kVer;
  o[3] = r.reason;
  put32(o + 4, r.seq);
  put32(o + 8, r.utc);
  put16(o + 12, static_cast<uint16_t>(r.day));
  put32(o + 14, r.odo);
  put32(o + 18, r.start);
  put32(o + 22, r.aboveS);
  put16(o + 30, crc16(o, 30));
}
bool OdoLog::decode(const uint8_t* in, Rec* r) {
  if (get16(in) != kMagic || in[2] != kVer || get16(in + 30) != crc16(in, 30)) return false;
  r->reason = in[3];
  r->seq = get32(in + 4);
  r->utc = get32(in + 8);
  r->day = get16(in + 12);
  r->odo = get32(in + 14);
  r->start = get32(in + 18);
  r->aboveS = get32(in + 22);
  return true;
}

OdoLog::DayEnt* OdoLog::slotOf(int32_t d) const {
  if (!tab_) return nullptr;
  int32_t i = d % ODO_TABLE_DAYS;
  if (i < 0) i += ODO_TABLE_DAYS;
  return &tab_[i];
}
void OdoLog::tabPut(int32_t d, uint32_t seq, uint32_t start, uint32_t odo, uint32_t aboveS) {
  DayEnt* e = slotOf(d);
  if (!e) return;
  if (e->day1 == d + 1) {
    if (static_cast<int32_t>(seq - e->seq) < 0) return;  // older record
  } else if (e->day1 != 0 && e->day1 - 1 > d) {
    return;  // a newer day is in that slot
  }
  e->day1 = d + 1;
  e->seq = seq;
  e->start = start;
  e->odo = odo;
  e->aboveS = aboveS;
  if (!haveEarliest_ || d < earliest_) {
    earliest_ = d;
    haveEarliest_ = true;
  }
}
// valid records of the sector at off (taken off the count before it is erased)
uint32_t OdoLog::countValid(uint32_t off) {
  if (!st_->read(off, buf_, ODO_SECTOR_BYTES)) return 0;
  uint32_t n = 0;
  Rec r;
  for (int i = 0; i < kSlotsPerSector; i++) n += decode(buf_ + i * ODO_REC_BYTES, &r) ? 1 : 0;
  return n;
}
bool OdoLog::eraseAt(uint32_t off) {
  const uint32_t n = countValid(off);
  if (!st_->erase(off, ODO_SECTOR_BYTES)) return false;
  records_ = records_ > n ? records_ - n : 0;
  return true;
}
bool OdoLog::writeRec(Rec r, uint32_t nowMs) {
  wrote_ = true;
  lastWriteMs_ = nowMs;
  if (!storageOk_ || slots_ == 0) return false;
  const uint32_t total = slots_ * ODO_REC_BYTES;
  if (pos_ >= total) pos_ = 0;
  if (pos_ % ODO_SECTOR_BYTES == 0) {
    if (!eraseAt(pos_)) {
      failures_++;
      return false;
    }
  } else {
    uint8_t cur[ODO_REC_BYTES];
    if (!st_->read(pos_, cur, ODO_REC_BYTES) || !blank(cur)) {  // not empty (power lost while writing): next sector
      pos_ = (pos_ / ODO_SECTOR_BYTES + 1) * ODO_SECTOR_BYTES;
      if (pos_ >= total) pos_ = 0;
      if (!eraseAt(pos_)) {
        failures_++;
        return false;
      }
    }
  }
  r.seq = ++seq_;
  uint8_t o[ODO_REC_BYTES], back[ODO_REC_BYTES];
  encode(r, o);
  const uint32_t at = pos_;
  pos_ += ODO_REC_BYTES;
  if (pos_ >= total) pos_ = 0;
  if (!st_->write(at, o, ODO_REC_BYTES) || !st_->read(at, back, ODO_REC_BYTES) ||
      std::memcmp(o, back, ODO_REC_BYTES) != 0) {
    failures_++;
    return false;
  }
  records_++;
  writes_++;
  tabPut(r.day, r.seq, r.start, r.odo, r.aboveS);
  last_ = r;
  haveLast_ = true;
  lwHave_ = true;
  lwDay_ = r.day;
  lwOdo_ = r.odo;
  lwAbove_ = r.aboveS;
  return true;
}

void OdoLog::resetState() {
  storageOk_ = false;
  slots_ = pos_ = seq_ = 0;
  records_ = writes_ = failures_ = 0;
  haveEarliest_ = haveLatest_ = haveLast_ = false;
  earliest_ = 0;
  dayKnown_ = startSet_ = aboveInit_ = false;
  curDay_ = 0;
  start_ = aboveDayMs_ = lastAbove_ = cur_ = 0;
  wrote_ = false;
  lastWriteMs_ = 0;
  closePending_ = false;
  lwHave_ = false;
  lwDay_ = 0;
  lwOdo_ = lwAbove_ = 0;
}

void OdoLog::begin(OdoStorage* storage, void* table) {
  resetState();
  st_ = storage;
  tab_ = static_cast<DayEnt*>(table);
  if (tab_) std::memset(tab_, 0, kTableBytes);
  const uint32_t size = st_ ? st_->size() : 0;
  slots_ = size / ODO_SECTOR_BYTES * kSlotsPerSector;
  if (slots_ == 0) return;
  bool readOk = true;
  uint32_t latestSlot = 0;
  for (uint32_t sec = 0; sec < slots_ / kSlotsPerSector; sec++) {
    if (!st_->read(sec * ODO_SECTOR_BYTES, buf_, ODO_SECTOR_BYTES)) {
      readOk = false;
      continue;
    }
    for (int i = 0; i < kSlotsPerSector; i++) {
      Rec r;
      if (!decode(buf_ + i * ODO_REC_BYTES, &r)) continue;
      records_++;
      tabPut(r.day, r.seq, r.start, r.odo, r.aboveS);
      if (!haveLatest_ || static_cast<int32_t>(r.seq - latest_.seq) > 0) {
        latest_ = r;
        haveLatest_ = true;
        latestSlot = sec * kSlotsPerSector + static_cast<uint32_t>(i);
      }
    }
  }
  storageOk_ = readOk;
  if (haveLatest_) {
    seq_ = latest_.seq;
    pos_ = ((latestSlot + 1) % slots_) * ODO_REC_BYTES;
    lwHave_ = true;
    lwDay_ = latest_.day;
    lwOdo_ = latest_.odo;
    lwAbove_ = latest_.aboveS;
    last_ = latest_;
    haveLast_ = true;
  }
}

void OdoLog::update(const Input& in, uint32_t nowMs) {
  // the local day
  if (in.timeOk) {
    const int32_t d = dayOfUtc(in.utc);
    if (!dayKnown_) {
      dayKnown_ = true;
      curDay_ = d;
      // the newest record is today's (an earlier power-up): continue its start odometer and over-rpm
      // time (what was counted before the time was known is added)
      if (haveLatest_ && latest_.day == d && (!in.odoHave || in.odoKm >= latest_.start)) {
        start_ = latest_.start;
        startSet_ = true;
        aboveDayMs_ += latest_.aboveS * 1000u;
      }
    } else if (d != curDay_) {
      // a new day: the closing record of the previous day (when it grew; written as soon as writing is
      // allowed) and the start of the new one
      if (startSet_) {
        Rec c;
        c.reason = R_CLOSE;
        c.seq = 0;
        c.utc = in.utc;
        c.day = curDay_;
        c.odo = cur_;
        c.start = start_;
        c.aboveS = aboveDayMs_ / 1000u;
        const uint32_t km = recKm(c);
        const bool changed = (!lwHave_ || lwDay_ != curDay_) ? (km > 0 || c.aboveS > 0)
                                                             : (c.odo > lwOdo_ || c.aboveS > lwAbove_);
        if (changed) {
          close_ = c;
          closePending_ = true;
          tabPut(c.day, seq_, c.start, c.odo, c.aboveS);  // in the table now (the write replaces it)
        }
        start_ = cur_;
      }
      curDay_ = d;
      aboveDayMs_ = 0;
    }
  }
  // over-rpm time: what in.aboveMs grew by
  if (!aboveInit_) {
    aboveInit_ = true;
  } else if (in.aboveMs >= lastAbove_) {
    aboveDayMs_ += in.aboveMs - lastAbove_;
  }
  lastAbove_ = in.aboveMs;
  // odometer
  if (in.odoHave) {
    cur_ = in.odoKm;
    if (!startSet_) {
      start_ = in.odoKm;
      startSet_ = true;
    }
  }
  // writing is allowed when slow (standing still included) or in reverse
  const bool low = in.speedOk && in.speedKmh <= ODO_LOW_KMH;
  if (!(low || in.reverse) || !storageOk_) return;
  const uint8_t reason = (in.speedOk && in.speedKmh < ODO_STILL_KMH) ? R_STILL : in.reverse ? R_REVERSE : R_LOW;
  if (closePending_) {  // the previous day's close (no interval)
    closePending_ = false;
    writeRec(close_, nowMs);
    return;
  }
  if (!in.timeOk || !dayKnown_ || !startSet_ || !in.odoOk) return;
  Rec r;
  r.reason = reason;
  r.seq = 0;
  r.utc = in.utc;
  r.day = curDay_;
  r.odo = cur_;
  r.start = start_;
  r.aboveS = aboveDayMs_ / 1000u;
  const uint32_t km = recKm(r);
  const bool changed = (!lwHave_ || lwDay_ != curDay_) ? (km > 0 || r.aboveS > 0)
                                                       : (r.odo > lwOdo_ || r.aboveS > lwAbove_);
  if (!changed) return;
  if (wrote_ && nowMs - lastWriteMs_ < ODO_MIN_INTERVAL_MS) return;
  writeRec(r, nowMs);
}

void OdoLog::status(Status* o) const {
  std::memset(o, 0, sizeof(*o));
  o->storageOk = storageOk_;
  o->records = records_;
  o->capacity = slots_;
  o->writes = writes_;
  o->failures = failures_;
  o->timeKnown = dayKnown_;
  o->today = curDay_;
  o->todayKm = startSet_ && cur_ >= start_ ? cur_ - start_ : 0;
  o->todayAboveS = aboveDayMs_ / 1000u;
  o->curOk = startSet_;
  o->cur = cur_;
  if (haveLast_) {
    o->lastUtc = last_.utc;
    o->lastReason = last_.reason;
    o->lastKm = recKm(last_);
    o->lastAboveS = last_.aboveS;
    o->lastOdo = last_.odo;
  }
}
bool OdoLog::wroteSince(uint32_t* writes) const {
  if (writes_ == *writes) return false;
  *writes = writes_;
  return true;
}

// day d (today also what is not written yet). start, odo = first and last odometer of the day
bool OdoLog::dayRec(int32_t d, uint32_t* start, uint32_t* odo, uint32_t* aboveS) const {
  bool rec = false;
  *start = *odo = *aboveS = 0;
  const DayEnt* e = slotOf(d);
  if (e && e->day1 == d + 1) {
    *start = e->start;
    *odo = e->odo;
    *aboveS = e->aboveS;
    rec = true;
  }
  if (dayKnown_ && d == curDay_ && startSet_) {  // today: the table has the values of the last write
    if (!rec || cur_ != *odo || aboveDayMs_ / 1000u != *aboveS) {
      *start = start_;
      *odo = cur_;
      *aboveS = aboveDayMs_ / 1000u;
      if (cur_ > start_ || *aboveS > 0) rec = true;
    }
  }
  return rec;
}
bool OdoLog::day(int32_t d, uint32_t* km, uint32_t* aboveS) const {
  uint32_t st, od;
  const bool rec = dayRec(d, &st, &od, aboveS);
  *km = od >= st ? od - st : 0;
  return rec;
}
int32_t OdoLog::earliest() const {
  if (haveEarliest_) return dayKnown_ && curDay_ < earliest_ ? curDay_ : earliest_;
  return dayKnown_ ? curDay_ : 0;
}

// day number <-> date (1970-01-01 = 0. H. Hinnant's days_from_civil / civil_from_days)
int32_t OdoLog::daysFrom(int y, int m, int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = static_cast<unsigned>((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}
void OdoLog::civil(int32_t z, int* y, int* m, int* d, int* wday) {
  const int32_t zz = z + 719468;
  const int era = (zz >= 0 ? zz : zz - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(zz - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const int dd = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  const int mm = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  *y = static_cast<int>(yoe) + era * 400 + (mm <= 2);
  *m = mm;
  *d = dd;
  int w = static_cast<int>((z + 4) % 7);  // 1970-01-01 was a Thursday
  if (w < 0) w += 7;
  *wday = w;
}

bool OdoLog::today(int32_t* d) const {
  if (!dayKnown_) return false;
  *d = curDay_;
  return true;
}
void OdoLog::bounds(int level, int32_t d, int32_t* first, int32_t* last) {
  int y, m, dd, w;
  civil(d, &y, &m, &dd, &w);
  if (level <= LV_MONTH) {
    const int m0 = level == LV_YEAR ? 1 : level == LV_QUARTER ? (m - 1) / 3 * 3 + 1 : m;  // first month
    const int n = level == LV_YEAR ? 12 : level == LV_QUARTER ? 3 : 1;                  // months
    const int mi = y * 12 + (m0 - 1) + n;  // first month of the next period
    *first = daysFrom(y, m0, 1);
    *last = daysFrom(mi / 12, mi % 12 + 1, 1) - 1;
  } else if (level == LV_WEEK) {
    *first = d - ((w - ODO_WEEK_START + 7) % 7);
    *last = *first + 6;
  } else {
    *first = *last = d;
  }
}
// odometer at the end of day d (that day's, or the nearest earlier day with a record; today: now)
bool OdoLog::odoAt(int32_t d, uint32_t* odo) const {
  if (!haveEarliest_ && !(dayKnown_ && startSet_)) return false;
  const int32_t lo = earliest();
  for (int32_t x = d; x >= lo && x > d - ODO_TABLE_DAYS; x--) {
    uint32_t st, a;
    if (dayRec(x, &st, odo, &a)) return true;
    if (dayKnown_ && x == curDay_ && startSet_) {  // today, nothing grew yet
      *odo = cur_;
      return true;
    }
  }
  return false;
}
bool OdoLog::periodAt(int level, int32_t anchor, Period* o) const {
  std::memset(o, 0, sizeof(*o));
  if (!dayKnown_) return false;
  const int32_t td = curDay_;
  if (anchor > td) anchor = td;
  bounds(level, anchor, &o->first, &o->last);
  const int32_t end = o->last < td ? o->last : td;
  o->current = o->last >= td;
  // end odometer, and the start (end of the day before; without an earlier record the start of the
  // first record in the period)
  o->odoKnown = odoAt(end, &o->odoEnd);
  bool startOk = odoAt(o->first - 1, &o->odoStart);
  for (int32_t d = o->first; d <= end; d++) {
    uint32_t st, od, a;
    if (!dayRec(d, &st, &od, &a)) continue;
    o->hasRecord = true;
    o->aboveS += a;
    if (!startOk) {
      o->odoStart = st;
      startOk = true;
    }
  }
  if (!startOk) o->odoStart = o->odoEnd;
  o->km = o->odoKnown && o->odoEnd >= o->odoStart ? o->odoEnd - o->odoStart : 0;
  o->canBack = earliest() < o->first;
  o->canFwd = o->last < td;
  return true;
}
int32_t OdoLog::prevAnchor(int level, int32_t anchor) {
  int32_t f, l;
  bounds(level, anchor, &f, &l);
  bounds(level, f - 1, &f, &l);
  return l;
}
int32_t OdoLog::nextAnchor(int level, int32_t anchor) const {
  int32_t f, l;
  bounds(level, anchor, &f, &l);
  bounds(level, l + 1, &f, &l);
  return dayKnown_ && l > curDay_ ? curDay_ : l;
}

// ---- OdoNav
void OdoNav::open(const OdoLog& log) {
  level_ = OdoLog::LV_YEAR;
  int32_t d = 0;
  anchor_ = log.today(&d) ? d : 0;
}
bool OdoNav::prev(const OdoLog& log) {
  OdoLog::Period p;
  if (!log.periodAt(level_, anchor_, &p) || !p.canBack) return false;
  anchor_ = OdoLog::prevAnchor(level_, anchor_);
  return true;
}
bool OdoNav::next(const OdoLog& log) {
  OdoLog::Period p;
  if (!log.periodAt(level_, anchor_, &p) || !p.canFwd) return false;
  anchor_ = log.nextAnchor(level_, anchor_);
  return true;
}
bool OdoNav::canFiner(const OdoLog& log) const {
  int32_t d;
  return level_ < OdoLog::LV_DAY && log.today(&d);
}
bool OdoNav::setLevel(int level, const OdoLog& log) {
  int32_t td;
  if (level < 0 || level >= OdoLog::LV_COUNT || !log.today(&td)) return false;
  if (anchor_ > td) anchor_ = td;
  if (level > level_) {  // finer: the last period inside the one shown (not after today)
    int32_t f, l;
    OdoLog::bounds(level_, anchor_, &f, &l);
    anchor_ = l < td ? l : td;
  }
  level_ = level;
  return true;
}

}  // namespace lt
