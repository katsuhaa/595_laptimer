// ui_glue.cpp - see ui_glue.h
#include "ui_glue.h"

#include <Arduino.h>
#include <FS.h>
#include <string.h>

#include <new>

#include "../hw/display.h"
#include "../hw/sdcard.h"
#include "../hw/touch.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "logger.h"
#include "lt_core.h"
#include "lt_ui.h"
#include "net.h"
#include "sessions.h"
#include "settings.h"
#include "state.h"
#include "timing.h"

namespace app {
settings::Settings& settingsRef();
}

namespace uiglue {
namespace {

constexpr int kJsonCap = 16 * 1024;  // courses.json / maint.json text

uint16_t* g_fb = nullptr;
float* g_scratch = nullptr;
uint16_t* g_layer = nullptr;
ui::Model* g_m = nullptr;
timing::View* g_tv = nullptr;  // the timing snapshot of the current frame
bool g_lcdOk = false;
uint8_t g_page = ui::SC_DRIVE;  // page chosen by swipes while touch is enabled
bool g_gateLoaded = false;

struct CfgLock {
  CfgLock() { xSemaphoreTake(static_cast<SemaphoreHandle_t>(app::lockRef()), portMAX_DELAY); }
  ~CfgLock() { xSemaphoreGive(static_cast<SemaphoreHandle_t>(app::lockRef())); }
};

void push(const ui::Dirty& d) {
  if (!g_lcdOk) return;
  for (int i = 0; i < d.n; ++i) {
    const ui::Rect& r = d.r[i];
    hw::displayPush(g_fb, r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
  }
}

// Model strings: cut at a UTF-8 character boundary (a cut character would be drawn as a box)
void copyStr(char* dst, size_t cap, const char* src) { ui::copyText(dst, cap, src ? src : ""); }

// Local start time of a session from its id YYYYMMDD_HHMMSS
bool idTime(const char* id, uint8_t* month, uint8_t* day, uint8_t* hour, uint8_t* minute) {
  if (!id || strlen(id) < 13 || id[8] != '_') return false;
  auto two = [&](int i) { return static_cast<uint8_t>((id[i] - '0') * 10 + (id[i + 1] - '0')); };
  for (int i = 0; i < 13; ++i) {
    if (i != 8 && (id[i] < '0' || id[i] > '9')) return false;
  }
  *month = two(4);
  *day = two(6);
  *hour = two(9);
  *minute = two(11);
  return true;
}

// ---- model pieces -----------------------------------------------------------------------------
void fillClock(const app::Live& s, uint32_t now, int tzMin, ui::Clock* c) {
  c->ok = s.timeOk;
  if (!s.timeOk) return;
  const int64_t utcMs = s.utcMsAtFix + static_cast<int64_t>(now - s.utcBaseMs);
  lt::gtime::DateTime dt;
  lt::gtime::toLocal(utcMs / 1000, tzMin, &dt);
  c->year = static_cast<uint16_t>(dt.year);
  c->month = static_cast<uint8_t>(dt.month);
  c->day = static_cast<uint8_t>(dt.day);
  c->hour = static_cast<uint8_t>(dt.hour);
  c->minute = static_cast<uint8_t>(dt.min);
  c->second = static_cast<uint8_t>(dt.sec);
}

void fillSelfTest(const app::Live& s, uint32_t allPassSince, uint32_t now) {
  ui::SelfTest& t = g_m->selftest;
  t.n = app::kSelfTestRows;
  for (int i = 0; i < app::kSelfTestRows; ++i) {
    const app::SelfTestRow& a = s.selftest[i];
    ui::SelfTestRow& r = t.row[i];
    copyStr(r.label, sizeof r.label, a.label);
    copyStr(r.value, sizeof r.value, a.value);
    r.result = a.state == app::SelfTestRow::Fail   ? ui::TR_FAIL
               : a.state == app::SelfTestRow::Wait ? ui::TR_WAIT
                                                   : ui::TR_PASS;  // Pass and Info rows
  }
  t.advanceS = -1;
  if (s.selftestAllPass && allPassSince) {
    const int left = 3 - static_cast<int>((now - allPassSince) / 1000);
    t.advanceS = static_cast<int8_t>(left < 0 ? 0 : left);
  }
}

void fillCircuit(const app::Live& s, const timing::View& tv, uint32_t now) {
  ui::Circuit& c = g_m->circuit;
  c.lap = static_cast<uint16_t>(tv.lapNo);
  c.sector = static_cast<uint8_t>(tv.sector);
  c.running = tv.running;
  c.lapMs = 0;
  if (tv.running && s.gnssUp) {
    // elapsed in the lap: lap-timer time of the newest fix + the time since it arrived
    const lt::Fix lf = lt::toLapFix(s.fix);
    const int64_t us = lf.tUs - tv.lapStartUs + static_cast<int64_t>(now - s.fixMs) * 1000;
    c.lapMs = us > 0 ? static_cast<uint32_t>(us / 1000) : 0;
  }
  c.lastOk = tv.lastLapMs >= 0;
  c.lastMs = c.lastOk ? static_cast<uint32_t>(tv.lastLapMs) : 0;
  c.bestOk = tv.bestLapMs >= 0;
  c.bestMs = c.bestOk ? static_cast<uint32_t>(tv.bestLapMs) : 0;
  c.deltaOk = tv.deltaOk;
  c.deltaS = tv.deltaS;
  c.ghostOk = tv.deltaOk;
  const float g = static_cast<float>(c.lapMs) - tv.deltaS * 1000.0f;
  c.ghostMs = g > 0 ? static_cast<uint32_t>(g) : 0;
}

void fillChecklist(const app::Live& s) {
  ui::Checklist& k = g_m->check;
  k.show = s.checklistVisible;
  k.n = 0;
  static const uint8_t map[lt::kChkCount] = {ui::CK_SPORT, ui::CK_ESC_OFF, ui::CK_OIL, ui::CK_RACECHRONO};
  for (int i = 0; i < lt::kChkCount; ++i) {
    if (!s.checklist.shown[i]) continue;
    k.item[k.n].id = map[i];
    k.item[k.n].ok = s.checklist.done[i];
    k.n++;
  }
  if (k.n == 0) k.show = false;
}

void fillMaint(const app::Live& s, int tzMin, uint32_t now) {
  ui::Maint& u = g_m->maint;
  lt::MaintNow mn;
  if (s.timeOk) mn.today = lt::dayOfUtcMs(s.utcMsAtFix + static_cast<int64_t>(now - s.utcBaseMs), tzMin);
  mn.odoOk = s.odoKm > 0;
  mn.odoKm = s.odoKm;
  mn.trackOk = true;
  mn.trackH = s.trackHours;
  static lt::MaintStatus st[lt::kMaintMaxItems];
  CfgLock l;
  lt::Maint* m = app::maintRef();
  const int n = m->evaluate(mn, st, lt::kMaintMaxItems);
  u.n = 0;
  for (int i = 0; i < n && u.n < 12; ++i) {
    const lt::MaintItem& it = m->item(st[i].index);
    ui::MaintItem& r = u.item[u.n++];
    copyStr(r.id, sizeof r.id, it.id);
    copyStr(r.name, sizeof r.name, it.name);
    const float rem = st[i].remaining;
    const bool over = st[i].state == lt::MaintState::Overdue;
    switch (st[i].unit) {
      case lt::MaintUnit::Km:
        snprintf(r.remain, sizeof r.remain, over ? "超過 %d km" : "残り %d km", static_cast<int>(over ? -rem : rem));
        break;
      case lt::MaintUnit::Days:
        snprintf(r.remain, sizeof r.remain, over ? "超過 %d 日" : "残り %d 日", static_cast<int>(over ? -rem : rem));
        break;
      case lt::MaintUnit::TrackH:
        snprintf(r.remain, sizeof r.remain, over ? "超過 %.1f h" : "残り %.1f h", over ? -rem : rem);
        break;
      default:
        copyStr(r.remain, sizeof r.remain, "記録なし");
        break;
    }
    r.fraction = st[i].fraction < 0 ? 0 : st[i].fraction > 1 ? 1 : st[i].fraction;
    r.state = over ? ui::MS_OVERDUE : st[i].state == lt::MaintState::Warn ? ui::MS_SOON : ui::MS_OK;
  }
  u.odoOk = mn.odoOk;
  u.odoKm = static_cast<uint32_t>(s.odoKm);
  u.trackH = s.trackHours;
}

void fillLaps(const timing::View& tv) {
  ui::Laps& L = g_m->laps;
  L.n = 0;
  for (int i = 0; i < tv.numLaps && L.n < 64; ++i) {
    const timing::LapRow& r = tv.laps[i];
    ui::LapRow& u = L.lap[L.n++];
    u.no = static_cast<uint16_t>(r.lapNo);
    u.ms = static_cast<uint32_t>(r.lapMs);
    u.valid = true;
    u.nSec = static_cast<uint8_t>(r.numSplits > 4 ? 4 : r.numSplits);
    for (int k = 0; k < u.nSec; ++k) u.secMs[k] = static_cast<uint32_t>(r.splitMs[k]);
  }
  copyStr(L.course, sizeof L.course, tv.courseName);
}

void fillLapsSession(const app::Live& s) {
  ui::Laps& L = g_m->laps;
  sessions::Info si;
  const char* id = s.sessionOpen ? s.sessionId : (sessions::get(0, &si) ? si.id : "");
  L.startOk = idTime(id, &L.month, &L.day, &L.hour, &L.minute);
}

void fillData(const app::Live& s, int tzMin) {
  ui::DataPage& d = g_m->data;
  net::Status ns;
  net::status(&ns);
  net::Names nc;
  net::names(&nc);
  copyStr(d.hostname, sizeof d.hostname, nc.hostname);
  d.wifiOk = ns.connected;
  copyStr(d.ip, sizeof d.ip, ns.ip);
  copyStr(d.ssid, sizeof d.ssid, ns.ssid);
  d.remainHours = s.sdOk ? static_cast<int16_t>(s.remainHours) : -1;
  d.undownloaded = static_cast<uint16_t>(s.undownloaded);
  d.nSessions = 0;
  for (int i = 0; i < 16; ++i) {
    sessions::Info si;
    if (!sessions::get(i, &si)) break;
    ui::SessionCard& c = d.session[d.nSessions++];
    lt::gtime::DateTime dt;
    lt::gtime::toLocal(si.startUtcMs / 1000, tzMin, &dt);
    c.month = static_cast<uint8_t>(dt.month);
    c.day = static_cast<uint8_t>(dt.day);
    c.hour = static_cast<uint8_t>(dt.hour);
    c.minute = static_cast<uint8_t>(dt.min);
    copyStr(c.course, sizeof c.course, si.name);
    c.laps = static_cast<uint16_t>(si.laps);
    c.bestOk = si.bestMs > 0;
    c.bestMs = si.bestMs > 0 ? static_cast<uint32_t>(si.bestMs) : 0;
    c.downloaded = si.downloaded;
  }
  d.nNetworks = 0;
  for (int i = 0; i < nc.count && d.nNetworks < 4; ++i) copyStr(d.network[d.nNetworks++], sizeof d.network[0], nc.ssid[i]);
}

// Wi-Fi page (settings > Wi-Fi): registered names, the scan, the join of the network saved last
void fillWifi() {
  ui::WifiPage& w = g_m->wifi;
  net::Names nn;
  net::names(&nn);
  w.nSaved = 0;
  for (int i = 0; i < nn.count && i < 8; ++i) copyStr(w.saved[w.nSaved++], sizeof w.saved[0], nn.ssid[i]);
  static net::ScanView sv;  // ui task only (about 0.6 KB)
  net::scanView(&sv);
  w.scanning = sv.scanning;
  w.scanOk = sv.ok;
  w.nFound = 0;
  for (int i = 0; i < sv.n && w.nFound < 16; ++i) {
    ui::WifiFound& f = w.found[w.nFound++];
    copyStr(f.ssid, sizeof f.ssid, sv.found[i].ssid);
    f.rssi = sv.found[i].rssi;
    f.locked = sv.found[i].locked;
    f.saved = false;
    for (int k = 0; k < nn.count; ++k) {
      if (strcmp(nn.ssid[k], sv.found[i].ssid) == 0) f.saved = true;
    }
  }
  net::Status ns;
  net::status(&ns);
  w.connected = ns.connected;
  copyStr(w.ssid, sizeof w.ssid, ns.ssid);
  copyStr(w.ip, sizeof w.ip, ns.ip);
  uint8_t j = net::J_IDLE;
  char js[33];
  net::joinView(&j, js);
  switch (j) {  // net::Join -> ui::WifiJoin
    case net::J_TRYING: w.join = ui::WJ_TRYING; break;
    case net::J_OK: w.join = ui::WJ_OK; break;
    case net::J_NOT_FOUND: w.join = ui::WJ_NOT_FOUND; break;
    case net::J_BAD_PASSWORD: w.join = ui::WJ_BAD_PASSWORD; break;
    case net::J_FAILED: w.join = ui::WJ_FAILED; break;
    default: w.join = ui::WJ_IDLE; break;
  }
  copyStr(w.joinSsid, sizeof w.joinSsid, js);
}

void fillSettings(const timing::View& tv) {
  const settings::Settings& st = app::settingsRef();
  ui::SettingsPage& p = g_m->settings;
  p.brightness = st.brightnessPct;
  p.courseAuto = true;
  p.courseIndex = -1;
  p.oilInvert = st.oilWarmBit == 0;
  p.recording = app::recording();
  CfgLock l;
  p.raceChrono = app::checklistRef().racechrono;
  lt::CourseDb* db = app::coursesRef();
  p.nCourses = 0;
  for (int i = 0; i < db->count() && p.nCourses < 8; ++i) copyStr(p.courses[p.nCourses++], sizeof p.courses[0], db->course(i).name);
  p.gateSet[0] = p.gateSet[1] = p.gateSet[2] = false;
  ui::GateMark* gm = g_m->gate.gate;
  for (int i = 0; i < 3; ++i) gm[i] = ui::GateMark{};
  const int ci = tv.haveCourse ? db->find(tv.courseId) : -1;
  if (ci >= 0) {
    const lt::Course& c = db->course(ci);
    int split = 0;
    for (int g = 0; g < c.numGates; ++g) {
      int k = -1;
      if (c.gates[g].type == lt::GateType::Sf) k = 0;
      else if (split < 2) k = 1 + split++;
      if (k < 0) continue;
      p.gateSet[k] = true;
      gm[k].set = true;
      gm[k].latE7 = c.gates[g].latE7;
      gm[k].lonE7 = c.gates[g].lonE7;
      gm[k].headingDeg = c.gates[g].headingDeg;
      gm[k].widthM = c.gates[g].widthM;
    }
  }
}

// Track of the newest session for the gate page: main log lat/lon at evenly spaced places of the file
// (a seek and one short read per point, the storage lock taken per read: the logger keeps writing)
// 走行記録 (settings > 走行記録, and the odometer on the settings row)
void fillOdo() {
  app::OdoSnap sn;
  app::odoSnapshot(&sn);
  ui::OdoPage& o = g_m->odo;
  o = ui::OdoPage{};
  o.storageOk = sn.st.storageOk;
  o.records = sn.st.records;
  o.curOk = sn.st.curOk;
  o.cur = sn.st.cur;
  o.sinceBootKm = sn.st.todayKm;  // before the date is known: since power-up
  o.dateOk = sn.dateOk;
  o.level = static_cast<uint8_t>(sn.level >= 0 && sn.level < ui::OL_COUNT ? sn.level : 0);
  if (!sn.dateOk) return;
  auto date = [](int32_t day, ui::OdoDate* d) {
    int y, m, dd, w;
    lt::OdoLog::civil(day, &y, &m, &dd, &w);
    d->y = static_cast<uint16_t>(y);
    d->m = static_cast<uint8_t>(m);
    d->d = static_cast<uint8_t>(dd);
    d->wday = static_cast<uint8_t>(w);
  };
  const lt::OdoLog::Period& p = sn.p;
  date(p.first, &o.first);
  date(p.last, &o.last);
  o.current = p.current;
  o.odoKnown = p.odoKnown;
  o.odoEnd = p.odoEnd;
  o.odoStart = p.odoStart;
  o.km = p.km;
  o.aboveS = p.aboveS;
  o.canBack = p.canBack;
  o.canFwd = p.canFwd;
  o.canFiner = sn.canFiner;
}

void loadGateTrack() {
  ui::GatePage& gp = g_m->gate;
  gp.n = 0;
  sessions::Info si;
  if (!sessions::get(0, &si)) return;
  copyStr(gp.course, sizeof gp.course, si.name);
  gp.startOk = idTime(si.id, &gp.month, &gp.day, &gp.hour, &gp.minute);
  char path[96];
  snprintf(path, sizeof path, "/sessions/%s/%s_%s.csv", si.id, si.id, si.cid);
  static char buf[768];  // ui task only; a main log row is about 200 bytes
  static lt::Replay rp;
  if (!hw::sdLock(3000)) return;
  fs::File f = hw::sd().open(path, FILE_READ);
  size_t size = 0, n = 0;
  if (f) {
    size = f.size();
    n = f.read(reinterpret_cast<uint8_t*>(buf), sizeof buf - 1);
  }
  hw::sdUnlock();
  if (!f) return;
  buf[n] = 0;
  char* nl = static_cast<char*>(memchr(buf, '\n', n));
  if (nl) {
    *nl = 0;
    rp.header(buf, static_cast<int>(nl - buf));
    const size_t body = static_cast<size_t>(nl - buf) + 1;
    const int capPts = static_cast<int>(sizeof gp.pt / sizeof gp.pt[0]);
    for (int k = 0; k < capPts && size > body; ++k) {
      const size_t pos = body + static_cast<size_t>((static_cast<uint64_t>(size - body) * static_cast<uint64_t>(k)) /
                                                    static_cast<uint64_t>(capPts));
      if (!hw::sdLock(1000)) break;
      n = f.seek(pos) ? f.read(reinterpret_cast<uint8_t*>(buf), sizeof buf - 1) : 0;
      hw::sdUnlock();
      buf[n] = 0;
      // the first complete line after pos
      char* a0 = k == 0 ? buf : static_cast<char*>(memchr(buf, '\n', n));
      if (!a0) continue;
      if (k != 0) ++a0;
      char* e = static_cast<char*>(memchr(a0, '\n', n - static_cast<size_t>(a0 - buf)));
      if (!e) continue;
      *e = 0;
      lt::ReplayRow r;
      if (rp.parse(a0, static_cast<int>(e - a0), &r) && r.row.posOk) gp.pt[gp.n++] = {r.row.latE7, r.row.lonE7};
    }
  }
  if (hw::sdLock(3000)) {
    f.close();
    hw::sdUnlock();
  }
}

// Drawing speed on the driving screen (実装計画 1-2), one serial line every 5 s:
// frames per second, draw (ui::frame) and send (panel) times. PASS = at least 25 frames/s.
void perf(bool drive, uint32_t nowMs, uint32_t drawUs, uint32_t pushUs) {
  static uint32_t since = 0, frames = 0, drawSum = 0, drawMax = 0, pushSum = 0, pushMax = 0;
  if (!drive) {
    since = 0;
    return;
  }
  if (!since) {
    since = nowMs ? nowMs : 1;
    frames = drawSum = drawMax = pushSum = pushMax = 0;
    return;
  }
  ++frames;
  drawSum += drawUs;
  pushSum += pushUs;
  if (drawUs > drawMax) drawMax = drawUs;
  if (pushUs > pushMax) pushMax = pushUs;
  const uint32_t dt = nowMs - since;
  if (dt < 5000) return;
  const float fps = static_cast<float>(frames) * 1000.0f / static_cast<float>(dt);
  Serial.printf("ui: %.1f frames/s, draw %.1f ms avg %.1f max, send %.1f ms avg %.1f max -> %s\n",
                static_cast<double>(fps), static_cast<double>(drawSum) / frames / 1000.0, drawMax / 1000.0,
                static_cast<double>(pushSum) / frames / 1000.0, pushMax / 1000.0, fps >= 25.0f ? "PASS" : "FAIL (< 25)");
  since = nowMs;
  frames = drawSum = drawMax = pushSum = pushMax = 0;
}

// ---- actions ----------------------------------------------------------------------------------
void setGate(const ui::Action& a) {
  const timing::View& tv = *g_tv;  // this frame's snapshot (ui task)
  app::PsBuf jb(kJsonCap);
  char* json = jb.as<char>();
  if (!json) return;
  int n = 0;
  {
    CfgLock l;
    lt::CourseDb* db = app::coursesRef();
    lt::Course c = {};
    // the active course, else the one made here before ("custom": its S/F first, then the splits)
    const int ci = tv.haveCourse ? db->find(tv.courseId) : db->find("custom");
    if (ci >= 0) {
      c = db->course(ci);
    } else {
      copyStr(c.id, sizeof c.id, "custom");
      copyStr(c.name, sizeof c.name, g_m->gate.course[0] ? g_m->gate.course : "カスタム");
      c.minLapS = lt::kDefaultMinLapS;
    }
    lt::CourseGate g = {};
    g.type = a.arg == 0 ? lt::GateType::Sf : lt::GateType::Split;
    snprintf(g.name, sizeof g.name, a.arg == 0 ? "Start/Finish" : "Split %d", a.arg);
    g.latE7 = a.latE7;
    g.lonE7 = a.lonE7;
    g.headingDeg = a.headingDeg;
    g.widthM = a.value > 0 ? static_cast<float>(a.value) : (a.arg == 0 ? lt::kSfWidthM : lt::kSplitWidthM);
    // replace the S/F, or the a.arg-th split, or append
    int split = 0, at = -1;
    for (int i = 0; i < c.numGates; ++i) {
      if (a.arg == 0 && c.gates[i].type == lt::GateType::Sf) at = i;
      if (a.arg > 0 && c.gates[i].type == lt::GateType::Split && ++split == a.arg) at = i;
    }
    if (at < 0 && c.numGates < lt::kCourseMaxGates) at = c.numGates++;
    if (at < 0) return;
    c.gates[at] = g;
    if (!c.centerGiven && c.numGates == 1) {
      c.centerLatE7 = g.latE7;
      c.centerLonE7 = g.lonE7;
    }
    if (!db->put(c)) return;
    n = db->toJson(json, kJsonCap);
  }
  if (n > 0) app::writeConfigFile("courses.json", json, n);
}

void setSetting(const ui::Action& a) {
  settings::Settings& st = app::settingsRef();
  switch (a.arg) {
    case ui::SET_SHIFT_GREEN:
    case ui::SET_SHIFT_YELLOW:
    case ui::SET_SHIFT_RED:
    case ui::SET_SHIFT_FLASH:
    case ui::SET_BRIGHTNESS:
    case ui::SET_OIL_INVERT: {
      CfgLock l;  // the house task saves a copy of the settings under the same lock
      if (a.arg == ui::SET_SHIFT_GREEN) st.shiftGreen = static_cast<int16_t>(a.value);
      if (a.arg == ui::SET_SHIFT_YELLOW) st.shiftYellow = static_cast<int16_t>(a.value);
      if (a.arg == ui::SET_SHIFT_RED) st.shiftRed = static_cast<int16_t>(a.value);
      if (a.arg == ui::SET_SHIFT_FLASH) st.shiftFlash = static_cast<int16_t>(a.value);
      if (a.arg == ui::SET_BRIGHTNESS) st.brightnessPct = static_cast<uint8_t>(a.value);
      if (a.arg == ui::SET_OIL_INVERT) st.oilWarmBit = a.value ? 0 : 1;
      break;
    }
    case ui::SET_RACECHRONO: {
      lt::ChecklistConfig c;
      {
        CfgLock l;
        c = app::checklistRef();
      }
      c.racechrono = a.value != 0;
      char json[256];
      const int n = lt::checklistJson(c, json, sizeof json);
      if (n > 0) app::writeConfigFile("checklist.json", json, n);
      return;
    }
    default:
      return;  // SET_COURSE: only the automatic choice (3 km rule) for now
  }
  app::settingsChanged();
}

void apply(const ui::Action& a, bool touchOn) {
  switch (a.kind) {
    case ui::ACT_PAGE:
      if (touchOn && a.arg < ui::SC_COUNT && a.arg != ui::SC_SELFTEST) {
        if (a.arg == ui::SC_ODO && g_page != ui::SC_ODO) app::odoNavOpen();  // this year
        g_page = a.arg;
        if (g_page == ui::SC_GATE) g_gateLoaded = false;
      }
      break;
    case ui::ACT_SELFTEST_SKIP: app::setPhaseRun(); break;
    case ui::ACT_MAINT_DONE: maintDone(a.id); break;
    case ui::ACT_GATE_SET: setGate(a); break;
    case ui::ACT_SETTING: setSetting(a); break;
    case ui::ACT_RECORD_STOP: app::setRecording(false); break;
    case ui::ACT_RECORD_RESUME: app::setRecording(true); break;
    case ui::ACT_WIFI_SCAN: net::requestScan(); break;
    case ui::ACT_WIFI_SAVE:
      // registered in NVS, first in the list, joined now (the page follows model.wifi.join)
      if (!net::saveNetwork(a.ssid, a.pass)) Serial.printf("wifi: could not register %s\n", a.ssid);
      break;
    case ui::ACT_WIFI_FORGET: net::forgetNetwork(a.ssid); break;
    case ui::ACT_ODO:
      if (touchOn) app::odoNavMove(a.arg, a.value);
      break;
    default: break;
  }
}

}  // namespace

// ---- public -----------------------------------------------------------------------------------
bool maintDone(const char* id) {
  app::Live s;
  app::liveGet(&s);
  const settings::Settings& st = app::settingsRef();
  lt::MaintNow mn;
  if (s.timeOk) mn.today = lt::dayOfUtcMs(s.utcMsAtFix + static_cast<int64_t>(millis() - s.utcBaseMs), st.tzMin);
  mn.odoOk = s.odoKm > 0;
  mn.odoKm = s.odoKm;
  mn.trackOk = true;
  mn.trackH = s.trackHours;
  app::PsBuf jb(kJsonCap);
  char* json = jb.as<char>();
  if (!json) return false;
  int n = 0;
  {
    CfgLock l;
    if (!app::maintRef()->markDone(id, mn)) return false;
    n = app::maintRef()->toJson(json, kJsonCap);
  }
  return n > 0 && app::writeConfigFile("maint.json", json, n);
}

bool courseFromVbo(const char* tmpPath, char* err, int errCap) {
  // Only the head of the file is needed: [laptiming] comes before [data] in RaceChrono's export
  static char* text = nullptr;
  constexpr size_t kCap = 256 * 1024;
  if (!text) text = static_cast<char*>(heap_caps_malloc(kCap + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!text) return false;
  size_t n = 0;
  if (!hw::sdLock(3000)) return false;
  fs::File f = hw::sd().open(tmpPath, FILE_READ);
  if (f) {
    n = f.read(reinterpret_cast<uint8_t*>(text), kCap);
    f.close();
  }
  hw::sdUnlock();
  text[n] = 0;
  char* data = strstr(text, "[data]");
  const int len = data ? static_cast<int>(data - text) : static_cast<int>(n);
  lt::Course c;
  const char* why = "";
  if (!lt::CourseDb::parseRaceChronoVbo(text, len, "course", &c, &why)) {
    if (err) snprintf(err, static_cast<size_t>(errCap), "%s", why);
    return false;
  }
  app::PsBuf jb(kJsonCap);
  char* json = jb.as<char>();
  if (!json) return false;
  int jn = 0;
  {
    CfgLock l;
    lt::CourseDb* db = app::coursesRef();
    // Same place as a registered course: replace it (keep its id and name); else a new id
    const int near = c.numGates ? db->nearest(c.gates[0].latE7, c.gates[0].lonE7) : -1;
    if (near >= 0) {
      copyStr(c.id, sizeof c.id, db->course(near).id);
      if (!c.name[0]) copyStr(c.name, sizeof c.name, db->course(near).name);
    } else {
      for (int k = db->count() + 1; k < 1000; ++k) {  // a free id (never another course's)
        snprintf(c.id, sizeof c.id, "course%d", k);
        if (db->find(c.id) < 0) break;
      }
    }
    if (!db->put(c)) {
      if (err) snprintf(err, static_cast<size_t>(errCap), "courses full");
      return false;
    }
    jn = db->toJson(json, kJsonCap);
  }
  return jn > 0 && app::writeConfigFile("courses.json", json, jn);
}

void begin(bool lcdOk) {
  g_lcdOk = lcdOk;
  g_fb = static_cast<uint16_t*>(heap_caps_malloc(ui::kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  g_scratch = static_cast<float*>(heap_caps_malloc(ui::kScratchBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  g_layer = static_cast<uint16_t*>(heap_caps_malloc(ui::kLayerBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  void* m = heap_caps_malloc(sizeof(ui::Model), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  void* tv = heap_caps_malloc(sizeof(timing::View), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (m) g_m = new (m) ui::Model();
  if (tv) g_tv = new (tv) timing::View();
  if (!g_fb || !g_scratch || !g_m || !g_tv) {
    g_m = nullptr;
    Serial.println("ui: PSRAM allocation failed");
    if (lcdOk) hw::displayFill(0xF800);  // red: like the round gauge, "no frame buffer"
    g_lcdOk = false;
    return;
  }
  ui::begin(g_fb, g_scratch, g_layer);
  copyStr(g_m->fwVersion, sizeof g_m->fwVersion, app::kFwVersion);
  g_m->screen = ui::SC_SELFTEST;
  g_m->touchEnabled = true;
  ui::Dirty d;
  ui::frame(*g_m, millis(), &d);
  push(d);
  if (lcdOk) hw::displayBrightness(app::settingsRef().brightnessPct);
}

void task(void*) {
  if (!g_m) vTaskDelete(nullptr);
  static ui::ActionQueue aq;
  static lt::PeakHold peak;
  bool wasDown = false;
  int lastX = 0, lastY = 0;
  uint32_t allPassSince = 0;
  int lastBright = -1;
  for (;;) {
    app::Live s;
    app::liveGet(&s);
    const uint32_t now = millis();  // after the snapshot: now - s.utcBaseMs / s.fixMs never wraps
    const settings::Settings& st = app::settingsRef();
    bool run = s.phase == app::Phase::Run;
    if (!run && !s.touchEnabled) {  // driving off (> 15 km/h) ends the self-test, passed or not
      app::setPhaseRun();
      run = true;
    }
    const bool touchOn = !run || s.touchEnabled;

    // touch -> ui actions
    int x = 0, y = 0;
    const bool down = hw::touchRead(&x, &y);
    aq.clear();
    if (down || wasDown) {
      ui::Input in;
      in.ms = now;
      in.kind = down ? (wasDown ? ui::IN_MOVE : ui::IN_DOWN) : ui::IN_UP;
      in.x = static_cast<int16_t>(down ? x : lastX);
      in.y = static_cast<int16_t>(down ? y : lastY);
      if (!(in.kind == ui::IN_MOVE && x == lastX && y == lastY)) ui::touch(in, now, *g_m, aq);
      if (down) {
        lastX = x;
        lastY = y;
      }
      wasDown = down;
    }
    for (int i = 0; i < aq.n; ++i) {
      apply(aq.a[i], touchOn);
      memset(aq.a[i].pass, 0, sizeof aq.a[i].pass);  // a typed password does not stay in memory
    }

    // screen
    if (!run) {
      g_m->screen = ui::SC_SELFTEST;
      if (s.selftestAllPass) {
        if (!allPassSince) allPassSince = now ? now : 1;
        if (now - allPassSince >= 3000) app::setPhaseRun();
      }
      fillSelfTest(s, allPassSince, now);
    } else {
      if (!s.touchEnabled) g_page = ui::SC_DRIVE;  // above 15 km/h: back to the driving screen
      g_m->screen = g_page;
    }
    g_m->drive = s.screen == lt::Screen::Circuit ? ui::DRIVE_CIRCUIT : ui::DRIVE_STREET;
    g_m->touchEnabled = touchOn;
    g_m->nightDim = s.lightOn;

    // common values
    const lt::VehValues& v = s.veh;
    peak.update(v.rpmOk, static_cast<float>(v.rpm), now);
    g_m->rpmOk = v.rpmOk;
    g_m->rpm = static_cast<uint16_t>(v.rpmOk && v.rpm > 0 ? v.rpm : 0);
    g_m->shift.green = static_cast<uint16_t>(st.shiftGreen);
    g_m->shift.yellow = static_cast<uint16_t>(st.shiftYellow);
    g_m->shift.red = static_cast<uint16_t>(st.shiftRed);
    g_m->shift.flash = static_cast<uint16_t>(st.shiftFlash);
    g_m->flash = v.rpmOk && v.rpm >= st.shiftFlash;
    g_m->peakVisible = peak.visible();
    g_m->peakMarkerRpm = static_cast<uint16_t>(peak.markerRpm() > 0 ? peak.markerRpm() : 0);
    g_m->peakTopRpm = static_cast<uint16_t>(peak.topRpm() > 0 ? peak.topRpm() : 0);
    g_m->speedOk = s.speedOk;
    g_m->speedKmh = s.speedKmh;
    g_m->gpsOk = s.gnssUp && s.fix.fixOk;
    g_m->hAccM = s.fix.hAcc_mm * 0.001f;
    g_m->sats = s.fix.numSV;
    logger::Stats ls;
    logger::stats(&ls);
    g_m->stLog = ls.errors ? ui::ST_ERR : ls.open ? ui::ST_OK : ui::ST_OFF;
    g_m->stCan = s.linkUp ? (s.canPerSec ? ui::ST_OK : ui::ST_WARN) : ui::ST_ERR;
    g_m->stBle = !s.bleOn ? ui::ST_OFF : s.bleConnected ? ui::ST_OK : ui::ST_WARN;
    net::Status ns;
    net::status(&ns);
    g_m->stWifi = ns.connected ? ui::ST_OK : ns.enabled ? ui::ST_WARN : ui::ST_OFF;
    g_m->oil = ui::OIL_UNKNOWN;  // the oil-temperature switch bit is not identified yet (UI設計 §5)
    fillClock(s, now, st.tzMin, &g_m->clock);
    timing::View* tv = g_tv;
    timing::view(tv);
    copyStr(g_m->courseName, sizeof g_m->courseName, tv->courseName);
    fillCircuit(s, *tv, now);
    fillChecklist(s);
    g_m->street.waterOk = v.waterOk;
    g_m->street.waterC = v.waterC;
    g_m->street.fuelOk = v.fuelOk;
    g_m->street.fuelPct = v.fuelPct;
    g_m->street.rangeOk = false;
    // pages (cheaper to fill only the one shown, plus maint for the street screen)
    if (g_m->screen == ui::SC_DRIVE || g_m->screen == ui::SC_MAINT || g_m->screen == ui::SC_SELFTEST) {
      fillMaint(s, st.tzMin, now);  // the self-test shows the most urgent item too
    }
    if (g_m->screen == ui::SC_LAPS) {
      fillLaps(*tv);
      fillLapsSession(s);
    }
    if (g_m->screen == ui::SC_DATA) fillData(s, st.tzMin);
    if (g_m->screen == ui::SC_SETTINGS || g_m->screen == ui::SC_GATE) fillSettings(*tv);
    if (g_m->screen == ui::SC_SETTINGS) fillData(s, st.tzMin);  // the Wi-Fi row shows the joined network
    if (g_m->screen == ui::SC_SETTINGS || g_m->screen == ui::SC_ODO) fillOdo();  // the row shows the ODO
    // the Wi-Fi page may switch the radio on (scan, join) also before the 30 s standstill
    net::setSetup(g_m->screen == ui::SC_WIFI);
    if (g_m->screen == ui::SC_WIFI) fillWifi();
    if (g_m->screen == ui::SC_GATE && !g_gateLoaded) {
      loadGateTrack();
      g_gateLoaded = true;
    }

    // brightness: the light switch selects the illumination brightness (round gauge 2.9.0)
    const int bright = s.lightOn ? st.illumPct : st.brightnessPct;
    if (bright != lastBright && g_lcdOk) {
      hw::displayBrightness(static_cast<uint8_t>(bright));
      lastBright = bright;
    }

    const uint32_t t0 = micros();
    ui::Dirty d;
    ui::frame(*g_m, now, &d);
    const uint32_t t1 = micros();
    push(d);
    const uint32_t t2 = micros();
    perf(g_m->screen == ui::SC_DRIVE, now, t1 - t0, t2 - t1);

    // 40 Hz on the driving screen (ring 25 Hz, hands 20 Hz), 20 Hz elsewhere; wake for the flash edge
    uint32_t wait = g_m->screen == ui::SC_DRIVE ? 25 : 50;
    const uint32_t due = ui::nextDueMs();
    if (due) {
      const uint32_t t = millis();
      const uint32_t dt = due > t ? due - t : 1;
      if (dt < wait) wait = dt;
    }
    vTaskDelay(pdMS_TO_TICKS(wait ? wait : 1));
  }
}

}  // namespace uiglue
