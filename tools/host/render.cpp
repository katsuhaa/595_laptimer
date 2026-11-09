// tools/host/render.cpp - draws every UI screen on the PC, writes PNGs, and checks the UI.
//
//   render [out dir]        (default out/render)
// Writes <out>/<scenario>.png for each scenario below and prints one PASS/FAIL line per check:
//   - incremental drawing == full redraw (with and without the cached layer) after a drive sequence
//   - touch scenario: swipes between the pages, end stops, vertical scroll, taps -> actions
//   - Wi-Fi page: list, scan, keyboard (every printable ASCII character), join results, by taps
//   - no missing glyphs, frame timings of the circuit screen (full and incremental)
// Build (repository root): c++ -std=c++17 -O2 -Wall -Wextra -Wshadow -I. ui/*.cpp tools/host/render*.cpp -o /tmp/ui_render
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <chrono>
#include <string>
#include <vector>

#include "tools/host/render_png.h"
#include "ui/lt_ui.h"

using namespace ui;

static uint16_t g_fb[kWidth * kHeight];
static uint16_t g_fb2[kWidth * kHeight];
static uint16_t g_layer[kWidth * kHeight];
static float g_scratch[kWidth * (kHeight + 2)];
static Model g_m;  // static: zero-initialised arrays
static std::string g_out = "out/render";
static int g_pass = 0, g_fail = 0;
static std::vector<std::string> g_pngs;

static void check(bool ok, const char *name, const char *detail = "") {
  printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name, detail[0] ? "  " : "", detail);
  (ok ? g_pass : g_fail)++;
}

static void copyStr(char *dst, size_t n, const char *s) {
  copyText(dst, n, s);
}

// ---------------------------------------------------------------- sample data
static void baseModel(Model &m) {
  m = Model();
  m.screen = SC_DRIVE;
  m.drive = DRIVE_CIRCUIT;
  m.touchEnabled = false;
  m.rpmOk = true;
  m.rpm = 5200;
  m.speedOk = true;
  m.speedKmh = 128;
  m.gpsOk = true;
  m.hAccM = 0.4f;
  m.sats = 14;
  m.stLog = ST_OK;
  m.stCan = ST_OK;
  m.stBle = ST_OK;
  m.stWifi = ST_OFF;
  m.oil = OIL_OK;
  m.clock = Clock{ true, 2026, 11, 22, 10, 32, 5 };
  copyStr(m.fwVersion, sizeof m.fwVersion, "0.1.0");
  copyStr(m.courseName, sizeof m.courseName, "袖ヶ浦フォレストレースウェイ");
  // self-test (canvas values; BLE: RaceChrono DIY only)
  static const char *const L[8] = { "GNSS", "XTSD", "CAN", "IMU", "TOUCH", "BLE", "WiFi", "5V" };
  static const char *const V[8] = { "25.0 Hz ・ 14 衛星 ・ hAcc 0.6 m", "1.84 GB ・ 空き 1.21 GB ・ 1 MB 書込→読戻し 一致", "214 フレーム/s ・ エラー 0",
                                    "QMI8658 ・ 200 Hz", "FT6336 応答", "RaceChrono DIY 購読 ○（待機）", "iPhone（共有） 172.20.10.3", "USB_5V 5.02 V" };
  m.selftest.n = 8;
  for (int i = 0; i < 8; i++) {
    copyStr(m.selftest.row[i].label, sizeof m.selftest.row[i].label, L[i]);
    copyStr(m.selftest.row[i].value, sizeof m.selftest.row[i].value, V[i]);
    m.selftest.row[i].result = TR_PASS;
  }
  m.selftest.advanceS = 3;
  // maintenance (canvas)
  struct Mi {
    const char *id, *name, *remain;
    float f;
    uint8_t st;
  } mi[5] = { { "fluid", "ブレーキフルード", "期限超過 12 日", 0.0f, MS_OVERDUE },
              { "oil", "エンジンオイル", "残り 320 km", 0.06f, MS_SOON },
              { "pad_f", "ブレーキパッド 前", "サーキット 残り 3.1 h", 0.31f, MS_OK },
              { "filter", "オイルフィルター", "残り 4,300 km", 0.43f, MS_OK },
              { "coolant", "冷却水", "残り 8 ヶ月", 0.33f, MS_OK } };
  m.maint.n = 5;
  for (int i = 0; i < 5; i++) {
    copyStr(m.maint.item[i].id, sizeof m.maint.item[i].id, mi[i].id);
    copyStr(m.maint.item[i].name, sizeof m.maint.item[i].name, mi[i].name);
    copyStr(m.maint.item[i].remain, sizeof m.maint.item[i].remain, mi[i].remain);
    m.maint.item[i].fraction = mi[i].f;
    m.maint.item[i].state = mi[i].st;
  }
  m.maint.odoOk = true;
  m.maint.odoKm = 48215;
  m.maint.trackH = 14.2f;
  // laps
  static const uint32_t lapMs[12] = { 101250, 95880, 94120, 93770, 93650, 93500, 93410, 93320, 93880, 95100, 93600, 94020 };
  m.laps.n = 12;
  for (int i = 0; i < 12; i++) {
    LapRow &r = m.laps.lap[i];
    r.no = (uint16_t)(i + 1);
    r.ms = lapMs[i];
    r.valid = i > 0;  // out lap
    r.nSec = 3;
    r.secMs[0] = lapMs[i] * 33 / 100;
    r.secMs[1] = lapMs[i] * 31 / 100;
    r.secMs[2] = lapMs[i] - r.secMs[0] - r.secMs[1];
  }
  copyStr(m.laps.course, sizeof m.laps.course, "袖ヶ浦");
  m.laps.startOk = true;
  m.laps.month = 11;
  m.laps.day = 22;
  m.laps.hour = 10;
  m.laps.minute = 32;
  // data
  copyStr(m.data.hostname, sizeof m.data.hostname, "laptimer");
  m.data.wifiOk = true;
  copyStr(m.data.ip, sizeof m.data.ip, "172.20.10.3");
  copyStr(m.data.ssid, sizeof m.data.ssid, "iPhone");
  m.data.remainHours = 26;
  m.data.undownloaded = 2;
  struct Sc {
    uint8_t mo, d, h, mi;
    const char *c;
    uint16_t laps;
    uint32_t best;
    bool dl;
  } sc[4] = { { 11, 22, 13, 5, "袖ヶ浦", 9, 93910, false }, { 11, 22, 10, 32, "袖ヶ浦", 12, 93320, true }, { 11, 15, 9, 50, "筑波", 15, 61880, false }, { 11, 3, 14, 20, "", 0, 0, true } };
  m.data.nSessions = 4;
  for (int i = 0; i < 4; i++) {
    SessionCard &s = m.data.session[i];
    s.month = sc[i].mo;
    s.day = sc[i].d;
    s.hour = sc[i].h;
    s.minute = sc[i].mi;
    copyStr(s.course, sizeof s.course, sc[i].c);
    s.laps = sc[i].laps;
    s.bestOk = sc[i].best != 0;
    s.bestMs = sc[i].best;
    s.downloaded = sc[i].dl;
  }
  m.data.nNetworks = 2;
  copyStr(m.data.network[0], sizeof m.data.network[0], "iPhone");
  copyStr(m.data.network[1], sizeof m.data.network[1], "home-2G");
  // settings
  m.settings.brightness = 80;
  m.settings.courseAuto = true;
  m.settings.nCourses = 3;
  copyStr(m.settings.courses[0], sizeof m.settings.courses[0], "袖ヶ浦フォレストレースウェイ");
  copyStr(m.settings.courses[1], sizeof m.settings.courses[1], "筑波サーキット コース2000");
  copyStr(m.settings.courses[2], sizeof m.settings.courses[2], "富士スピードウェイ");
  m.settings.gateSet[0] = m.settings.gateSet[1] = m.settings.gateSet[2] = true;
  m.settings.raceChrono = true;
  m.settings.recording = true;
  // gate page: a synthetic closed track around the course in ARCH 5.4 (not the owner's data)
  GatePage &gp = m.gate;
  gp.n = 420;
  const double lat0 = 35.3949090, lon0 = 140.0880580;
  for (int i = 0; i < gp.n; i++) {
    const double t = 2.0 * 3.14159265358979 * i / gp.n;
    const double x = 520.0 * cos(t) + 140.0 * cos(2 * t + 0.6) + 60.0 * sin(3 * t);
    const double y = 300.0 * sin(t) + 90.0 * sin(2 * t) - 40.0 * cos(3 * t + 0.3);
    gp.pt[i].latE7 = (int32_t)llround((lat0 + y / 111320.0) * 1e7);
    gp.pt[i].lonE7 = (int32_t)llround((lon0 + x / (111320.0 * cos(lat0 * 3.14159265358979 / 180.0))) * 1e7);
  }
  auto gate = [&](int k, int idx) {
    const int a = (idx + gp.n - 1) % gp.n, b = (idx + 1) % gp.n;
    const double dx = (gp.pt[b].lonE7 - gp.pt[a].lonE7) * cos(lat0 * 3.14159265358979 / 180.0), dy = gp.pt[b].latE7 - gp.pt[a].latE7;
    gp.gate[k].set = true;
    gp.gate[k].latE7 = gp.pt[idx].latE7;
    gp.gate[k].lonE7 = gp.pt[idx].lonE7;
    float h = (float)(atan2(dx, dy) * 180.0 / 3.14159265358979);
    gp.gate[k].headingDeg = h < 0 ? h + 360.0f : h;
    gp.gate[k].widthM = 30;
  };
  gate(0, 100);
  gate(1, 230);
  copyStr(gp.course, sizeof gp.course, "袖ヶ浦");
  gp.startOk = true;
  gp.month = 11;
  gp.day = 22;
  gp.hour = 10;
  gp.minute = 32;
}

static void lapModel(Model &m) {  // circuit_lap: rpm 5200, peak 6450, lap 7 at 38.4 s, ghost 38.9 s, delta -0.57
  baseModel(m);
  Circuit &c = m.circuit;
  c.lap = 7;
  c.sector = 2;
  c.running = true;
  c.lapMs = 38400;
  c.lastOk = true;
  c.lastMs = 93980;
  c.bestOk = true;
  c.bestMs = 93320;
  c.ghostOk = true;
  c.ghostMs = 38900;
  c.deltaOk = true;
  c.deltaS = -0.57f;
  m.peakVisible = true;
  m.peakMarkerRpm = 6450;
  m.peakTopRpm = 6450;
}

// ---------------------------------------------------------------- shots
static uint32_t g_now = 100000;
static bool savePng(const char *name) {
  std::string path = g_out + "/" + name + ".png";
  bool ok = render::writePng565(path, g_fb, kWidth, kHeight);
  if (ok) g_pngs.push_back(path);
  else check(false, "write png", path.c_str());
  return ok;
}
// one frame of the model; when the screen is new, the view state (optional) is applied and drawn again
static void show(const Model &m, const ViewState *vs = nullptr) {
  Dirty d;
  frame(m, g_now, &d);
  if (vs) {
    ViewState v = *vs;
    testSetView(v);
    frame(m, g_now, &d);
  }
}
// incremental result == full redraw (same model, same time)
static bool sameAsFull(const Model &m, const char *what) {
  memcpy(g_fb2, g_fb, sizeof g_fb);
  invalidate();
  Dirty d;
  frame(m, g_now, &d);
  int diff = 0, fx = -1, fy = -1;
  for (int i = 0; i < kWidth * kHeight; i++)
    if (g_fb[i] != g_fb2[i]) {
      if (!diff) {
        fx = i % kWidth;
        fy = i / kWidth;
      }
      diff++;
    }
  char det[96];
  snprintf(det, sizeof det, "%d pixels differ (first at %d,%d)", diff, fx, fy);
  check(diff == 0, what, diff ? det : "");
  return diff == 0;
}
static void shot(const char *name, const Model &m, const ViewState *vs = nullptr) {
  show(m, vs);
  savePng(name);
}
static ViewState view(uint8_t screen) {
  ViewState v;
  memset(&v, 0, sizeof v);
  v.screen = screen;
  v.scroll = 0;
  v.lapOpen = -1;
  v.maintDialog = -1;
  v.editor = -1;
  v.gatePick = -1;
  return v;
}

// a short drive on the circuit screen: rpm ramps, the lap clock runs, the marker moves
static void driveSequence(Model &m, int frames) {
  for (int i = 0; i < frames; i++) {
    g_now += 40;
    m.circuit.lapMs += 40;
    m.circuit.ghostMs += 40;
    m.rpm = (uint16_t)(4200 + (i * 37) % 2300);
    m.peakMarkerRpm = m.rpm > 5000 ? m.rpm : 5000;
    m.peakTopRpm = m.peakMarkerRpm;
    m.circuit.deltaS = -0.57f + 0.01f * (float)(i % 30);
    m.clock.minute = (uint8_t)(32 + i / 50);
    m.flash = m.rpm >= m.shift.flash;
    Dirty d;
    frame(m, g_now, &d);
  }
}

// ---------------------------------------------------------------- touch scenario
struct Touch {
  ActionQueue q;
  Model *m;
  void tap(int x, int y) {
    touch(Input{ IN_DOWN, (int16_t)x, (int16_t)y, g_now }, g_now, *m, q);
    g_now += 90;
    touch(Input{ IN_UP, (int16_t)x, (int16_t)y, g_now }, g_now, *m, q);
    apply();
  }
  void swipe(int x0, int y0, int x1, int y1) {
    touch(Input{ IN_DOWN, (int16_t)x0, (int16_t)y0, g_now }, g_now, *m, q);
    for (int k = 1; k <= 6; k++) {
      g_now += 16;
      touch(Input{ IN_MOVE, (int16_t)(x0 + (x1 - x0) * k / 6), (int16_t)(y0 + (y1 - y0) * k / 6), g_now }, g_now, *m, q);
    }
    g_now += 16;
    touch(Input{ IN_UP, (int16_t)x1, (int16_t)y1, g_now }, g_now, *m, q);
    apply();
  }
  ActionQueue last;
  std::vector<Action> all;  // every action since all.clear()
  void apply() {  // what the app does: page actions change the screen; then a frame is drawn
    last = q;
    for (int i = 0; i < q.n; i++) {
      all.push_back(q.a[i]);
      if (q.a[i].kind == ACT_PAGE) m->screen = q.a[i].arg;
    }
    q.clear();
    Dirty d;
    frame(*m, g_now, &d);
  }
  bool got(uint8_t kind) const {
    for (int i = 0; i < last.n; i++)
      if (last.a[i].kind == kind) return true;
    return false;
  }
  const Action *find(uint8_t kind) const {
    for (int i = 0; i < last.n; i++)
      if (last.a[i].kind == kind) return &last.a[i];
    return nullptr;
  }
};

static void touchTests() {
  Model &m = g_m;
  baseModel(m);
  m.screen = SC_DRIVE;
  m.touchEnabled = true;
  Touch t;
  t.m = &m;
  Dirty d;
  frame(m, g_now, &d);
  t.swipe(300, 300, 300, 300);  // no move: tap on the drive screen = nothing
  check(t.last.n == 0, "touch: tap on the drive screen does nothing");
  t.swipe(500, 220, 380, 226);
  check(m.screen == SC_LAPS, "touch: swipe left drive -> laps");
  t.swipe(500, 220, 380, 226);
  check(m.screen == SC_MAINT, "touch: swipe left laps -> maint");
  t.swipe(500, 220, 380, 226);
  check(m.screen == SC_DATA, "touch: swipe left maint -> data");
  t.swipe(500, 220, 380, 226);
  check(m.screen == SC_SETTINGS, "touch: swipe left data -> settings");
  t.swipe(500, 220, 380, 226);
  check(m.screen == SC_SETTINGS && t.last.n == 0, "touch: swipe left at settings stops (end)");
  t.swipe(200, 220, 330, 214);
  check(m.screen == SC_DATA, "touch: swipe right settings -> data");
  t.swipe(300, 220, 340, 220);  // 40 px: below the 50 px threshold
  check(m.screen == SC_DATA && t.last.n == 0, "touch: 40 px swipe is ignored");
  m.screen = SC_DRIVE;
  frame(m, g_now, &d);
  t.swipe(200, 220, 330, 214);
  check(m.screen == SC_DRIVE && t.last.n == 0, "touch: swipe right at the drive screen stops (end)");
  // vertical scroll on the laps page
  m.screen = SC_LAPS;
  frame(m, g_now, &d);
  const int s0 = viewState().scroll;
  t.swipe(300, 200, 300, 300);  // finger down: towards the first laps
  const int s1 = viewState().scroll;
  check(s1 == s0 - 100 && m.screen == SC_LAPS, "touch: vertical drag scrolls the laps rows by 100 px");
  // tap a row: sectors open, tap again: closed
  const int rowY = 66 + 76 * 2 + 30;  // third visible row
  t.tap(300, rowY);
  const int opened = viewState().lapOpen;
  check(opened >= 0, "touch: tap on a lap row opens its sectors");
  t.tap(300, rowY);
  check(viewState().lapOpen == -1, "touch: second tap closes the sectors");
  // short touch is ignored
  touch(Input{ IN_DOWN, 300, (int16_t)rowY, g_now }, g_now, m, t.q);
  g_now += 30;
  touch(Input{ IN_UP, 300, (int16_t)rowY, g_now }, g_now, m, t.q);
  t.apply();
  check(viewState().lapOpen == -1, "touch: a 30 ms touch is ignored");
  // maintenance: dialog and 実施済みにする
  m.screen = SC_MAINT;
  frame(m, g_now, &d);
  t.tap(300, 66 + 76 + 38);  // second item: エンジンオイル
  check(viewState().maintDialog == 1, "touch: tap on a maintenance row opens the dialog");
  t.swipe(500, 220, 380, 226);
  check(m.screen == SC_MAINT, "touch: no page swipe while the dialog is open");
  t.tap(170, 112 + 300 - 20 - 36);  // 実施済みにする
  const Action *a = t.find(ACT_MAINT_DONE);
  check(a && strcmp(a->id, "oil") == 0 && a->arg == 1 && viewState().maintDialog == -1, "touch: 実施済みにする -> ACT_MAINT_DONE(oil), dialog closed");
  t.tap(300, 66 + 38);
  t.tap(430, 112 + 300 - 20 - 36);  // 閉じる
  check(viewState().maintDialog == -1 && !t.got(ACT_MAINT_DONE), "touch: 閉じる closes the dialog without an action");
  // settings: shift editor +100 on the red point, gate page
  m.screen = SC_SETTINGS;
  frame(m, g_now, &d);
  t.tap(300, 66 + 76 * 3 + 38);  // シフトポイント (走行記録 is the first row)
  check(viewState().editor == 3, "touch: tap シフトポイント opens the editor");
  const int scr = viewState().scroll;
  const int edTop = 66 + 76 * 4 - scr;  // editor below the row
  t.tap(20 + 2 * 144 + 64, edTop + 12 + 28);  // red chip
  check(viewState().shiftSel == 2, "touch: tap the red value selects it");
  t.tap(300, edTop + 80 + 32);  // +100
  a = t.find(ACT_SETTING);
  check(a && a->arg == SET_SHIFT_RED && a->value == 6100, "touch: +100 -> ACT_SETTING(SET_SHIFT_RED, 6100)");
  t.tap(100, edTop + 80 + 32);  // −100
  a = t.find(ACT_SETTING);
  check(a && a->arg == SET_SHIFT_RED && a->value == 5900, "touch: −100 -> ACT_SETTING(SET_SHIFT_RED, 5900)");
  m.shift.red = 6300;  // at the flash point: +100 would pass it
  t.tap(300, edTop + 80 + 32);
  check(!t.got(ACT_SETTING), "touch: red cannot go above the flash point");
  m.shift.red = 6000;
  t.tap(490, edTop + 80 + 32);  // 閉じる
  check(viewState().editor == -1, "touch: 閉じる closes the shift editor");
  t.swipe(300, 300, 300, 100);  // scroll to the top again
  t.swipe(300, 100, 300, 400);
  t.tap(300, 66 + 76 * 2 + 38 - viewState().scroll);  // ゲート設定
  check(m.screen == SC_GATE, "touch: tap ゲート設定 -> gate page");
  // gate: pick a track point, set S2, back
  t.tap(212, 249);  // somewhere: may miss the track
  int pick = viewState().gatePick;
  if (pick < 0) {  // find a point of the track on screen by scanning
    for (int y = 60; y < 430 && pick < 0; y += 6)
      for (int x = 10; x < 420 && pick < 0; x += 6) {
        t.tap(x, y);
        pick = viewState().gatePick;
      }
  }
  check(pick >= 0, "touch: tap on the track picks a point");
  t.tap(428 + 78, 306 + 30);  // S2 に設定
  a = t.find(ACT_GATE_SET);
  check(a && a->arg == 2 && a->latE7 == m.gate.pt[pick].latE7 && a->value == 30 && viewState().gatePick == -1, "touch: S2 に設定 -> ACT_GATE_SET(S2, lat, lon, 30 m)");
  t.tap(428 + 78, 170 + 30);  // S/F without a pick: nothing
  check(!t.got(ACT_GATE_SET), "touch: set buttons do nothing without a picked point");
  t.tap(40, 24);  // ‹ 設定
  check(m.screen == SC_SETTINGS, "touch: ‹ 設定 -> settings");
  // touch disabled
  m.touchEnabled = false;
  t.swipe(200, 220, 330, 214);
  check(m.screen == SC_SETTINGS && t.last.n == 0, "touch: ignored while touch is disabled");
  // self-test tap
  m.touchEnabled = true;
  m.screen = SC_SELFTEST;
  frame(m, g_now, &d);
  t.tap(300, 200);
  check(t.got(ACT_SELFTEST_SKIP), "touch: tap on the self-test -> ACT_SELFTEST_SKIP");
  // record toggle
  m.screen = SC_SETTINGS;
  frame(m, g_now, &d);
  t.swipe(300, 400, 300, 60);  // scroll down
  t.swipe(300, 400, 300, 60);
  const int sc2 = viewState().scroll;
  t.tap(300, 66 + 76 * 7 + 38 - sc2);  // 記録
  check(t.got(ACT_RECORD_STOP), "touch: tap 記録 while recording -> ACT_RECORD_STOP");
}

// ---------------------------------------------------------------- Wi-Fi page (settings > Wi-Fi)
static void wifiSample(Model &m) {
  WifiPage &w = m.wifi;
  w = WifiPage();
  static const char *const SAVED[3] = { "iPhone", "home-2G", "Aterm-5A2B3C-g" };
  w.nSaved = 3;
  for (int i = 0; i < 3; i++) copyStr(w.saved[i], sizeof w.saved[i], SAVED[i]);
  w.connected = true;
  copyStr(w.ssid, sizeof w.ssid, "iPhone");
  copyStr(w.ip, sizeof w.ip, "172.20.10.3");
  struct F {
    const char *s;
    int8_t rssi;
    bool locked, saved;
  } f[6] = { { "iPhone", -48, true, true },          { "Aterm-5A2B3C-g", -61, true, true }, { "鈴木のiPhone", -66, true, false },
             { "Buffalo-G-1A2B", -72, true, false }, { "FREE_WiFi", -79, false, false },    { "サーキット_Guest", -86, false, false } };
  w.nFound = 6;
  for (int i = 0; i < 6; i++) {
    copyStr(w.found[i].ssid, sizeof w.found[i].ssid, f[i].s);
    w.found[i].rssi = f[i].rssi;
    w.found[i].locked = f[i].locked;
    w.found[i].saved = f[i].saved;
  }
  w.scanOk = true;
}
static void tapKey(Touch &t, uint32_t code) {
  int16_t x, y;
  if (testWifiKey(viewState().wifiLayer, code, &x, &y)) t.tap(x, y);
  else check(false, "wifi: control key not found");
}
// taps the keys of s on the keyboard, switching layers on the way; false if a character has no key
static bool typeKeys(Touch &t, const char *s) {
  for (; *s; s++) {
    const uint32_t c = (uint8_t)*s;
    int16_t x, y;
    int layer = viewState().wifiLayer, target = -1;
    for (int l = 0; l < 3 && target < 0; l++) {
      const int k = (layer + l) % 3;  // the current layer first
      if (testWifiKey((uint8_t)k, c, &x, &y)) target = k;
    }
    if (target < 0) return false;
    if ((target == 2) != (layer == 2)) {  // letters <-> symbols
      tapKey(t, TK_LAYER);
      layer = viewState().wifiLayer;
    }
    if (target != layer) {  // lower <-> upper
      tapKey(t, TK_SHIFT);
      layer = viewState().wifiLayer;
    }
    if (layer != target || !testWifiKey((uint8_t)layer, c, &x, &y)) return false;
    t.tap(x, y);
  }
  return true;
}
static void backspaces(Touch &t, int n) {
  for (int i = 0; i < n; i++) tapKey(t, TK_BACK);
}
static int countKind(const std::vector<Action> &v, uint8_t kind) {
  int n = 0;
  for (const Action &a : v) n += a.kind == kind;
  return n;
}
static const Action *lastKind(const std::vector<Action> &v, uint8_t kind) {
  for (size_t i = v.size(); i-- > 0;)
    if (v[i].kind == kind) return &v[i];
  return nullptr;
}
static std::vector<uint16_t> band() {  // rows 0..66: the header band (on the keyboard: the text field)
  return std::vector<uint16_t>(g_fb, g_fb + kWidth * 66);
}
static bool screenIs(const std::vector<uint16_t> &a) {
  return memcmp(a.data(), g_fb, sizeof g_fb) == 0;
}

static void wifiTests() {
  Model &m = g_m;
  baseModel(m);
  wifiSample(m);
  m.touchEnabled = true;
  Touch t;
  t.m = &m;
  Dirty d;
  char det[160];
  // settings: the Wi-Fi row (last but one) opens the page
  m.screen = SC_SETTINGS;
  frame(m, g_now, &d);
  t.swipe(300, 400, 300, 60);
  t.swipe(300, 400, 300, 60);
  shot("page_settings_wifi_row", m);
  t.tap(300, 66 + 76 * 8 + 38 - viewState().scroll);
  const Action *a = t.find(ACT_PAGE);
  check(a && a->arg == SC_WIFI && m.screen == SC_WIFI && viewState().wifiView == 0, "wifi: settings Wi-Fi row -> ACT_PAGE(SC_WIFI), list view");
  shot("wifi_list_connected", m);
  sameAsFull(m, "wifi_list_connected: incremental == full");
  t.swipe(500, 220, 380, 226);
  const bool noLeft = m.screen == SC_WIFI && t.last.n == 0;
  t.swipe(200, 220, 330, 214);
  check(noLeft && m.screen == SC_WIFI && t.last.n == 0 && viewState().wifiDialog == 0, "wifi: no page swipe on SC_WIFI (left, right)");
  m.touchEnabled = false;
  t.tap(300, 66 + 92 + 38);  // first network: would open the dialog
  t.tap(60, 32);             // ‹ 設定
  check(t.last.n == 0 && viewState().wifiDialog == 0 && m.screen == SC_WIFI, "wifi: taps ignored while touch is disabled");
  m.touchEnabled = true;
  // delete dialog
  t.tap(300, 66 + 92 + 76 + 38);  // home-2G
  check(viewState().wifiDialog == 1, "wifi: tap a registered network opens the delete dialog");
  shot("wifi_forget_dialog", m);
  sameAsFull(m, "wifi_forget_dialog: incremental == full");
  t.swipe(300, 300, 300, 120);
  check(viewState().scroll == 0 && viewState().wifiDialog == 1, "wifi: no scroll while the dialog is open");
  t.all.clear();
  t.tap(425, 356);  // キャンセル
  check(viewState().wifiDialog == 0 && t.all.empty(), "wifi: キャンセル closes the delete dialog without an action");
  t.tap(300, 66 + 92 + 76 + 38);
  t.tap(175, 356);  // 削除
  a = lastKind(t.all, ACT_WIFI_FORGET);
  check(a && strcmp(a->ssid, "home-2G") == 0 && countKind(t.all, ACT_WIFI_FORGET) == 1 && viewState().wifiDialog == 0, "wifi: 削除 -> ACT_WIFI_FORGET(home-2G)");
  sameAsFull(m, "wifi list after the dialog: incremental == full");
  // empty list, full list (8: adding disabled, scrolls)
  m.wifi.nSaved = 0;
  m.wifi.connected = false;
  m.wifi.ssid[0] = m.wifi.ip[0] = 0;
  shot("wifi_list_empty", m);
  sameAsFull(m, "wifi_list_empty: incremental == full");
  static const char *const MORE[8] = { "iPhone", "home-2G", "Aterm-5A2B3C-g", "Buffalo-G-1A2B", "鈴木のiPhone", "office_5F", "サーキット_Guest", "car-hotspot" };
  m.wifi.nSaved = 8;
  for (int i = 0; i < 8; i++) copyStr(m.wifi.saved[i], sizeof m.wifi.saved[i], MORE[i]);
  m.wifi.connected = true;
  copyStr(m.wifi.ssid, sizeof m.wifi.ssid, "office_5F");
  copyStr(m.wifi.ip, sizeof m.wifi.ip, "192.168.10.24");
  frame(m, g_now, &d);
  t.swipe(300, 400, 300, 100);
  t.swipe(300, 400, 300, 100);
  const int sMax = 92 + 8 * 76 + 76 - 384;
  check(viewState().scroll == sMax, "wifi: the list scrolls (8 networks)");
  shot("wifi_list_full", m);
  sameAsFull(m, "wifi_list_full: incremental == full");
  t.all.clear();
  t.tap(300, 450 - 38);  // ＋ ネットワークを追加 (disabled: 8 件まで)
  check(t.all.empty() && viewState().wifiView == 0, "wifi: 8 networks: adding is disabled");
  wifiSample(m);
  frame(m, g_now, &d);
  // scan
  m.wifi.scanOk = false;
  t.swipe(300, 120, 300, 420);  // back to the top
  t.tap(300, 66 + 92 + 3 * 76 + 20);  // ＋ ネットワークを追加
  check(t.got(ACT_WIFI_SCAN) && viewState().wifiView == 1, "wifi: ＋ ネットワークを追加 -> ACT_WIFI_SCAN, scan view");
  m.wifi.scanning = true;
  shot("wifi_scan_searching", m);
  sameAsFull(m, "wifi_scan_searching: incremental == full");
  t.tap(518, 32);
  check(!t.got(ACT_WIFI_SCAN), "wifi: 再検索 does nothing while searching");
  m.wifi.scanning = false;
  m.wifi.scanOk = true;
  shot("wifi_scan_results", m);
  sameAsFull(m, "wifi_scan_results: incremental == full");
  t.tap(518, 32);
  check(t.got(ACT_WIFI_SCAN) && viewState().wifiView == 1, "wifi: 再検索 -> ACT_WIFI_SCAN");
  // open network: confirm dialog -> ACT_WIFI_SAVE(ssid, "")
  t.tap(300, 384);  // FREE_WiFi (row 5, partly above the hint)
  check(viewState().wifiDialog == 2, "wifi: tap an open network opens the confirm dialog");
  shot("wifi_open_dialog", m);
  sameAsFull(m, "wifi_open_dialog: incremental == full");
  t.all.clear();
  t.tap(175, 356);  // 登録する
  a = lastKind(t.all, ACT_WIFI_SAVE);
  check(a && countKind(t.all, ACT_WIFI_SAVE) == 1 && strcmp(a->ssid, "FREE_WiFi") == 0 && a->pass[0] == 0 && viewState().wifiView == 3,
        "wifi: 登録する -> one ACT_WIFI_SAVE(FREE_WiFi, \"\"), result view");
  m.wifi.join = WJ_FAILED;
  copyStr(m.wifi.joinSsid, sizeof m.wifi.joinSsid, "FREE_WiFi");
  shot("wifi_result_failed_open", m);
  sameAsFull(m, "wifi_result_failed_open: incremental == full");
  t.tap(300, 398);  // OK (no 入力しなおす for an open network)
  check(viewState().wifiView == 0, "wifi: open network failed: OK -> list");
  // locked network: keyboard
  m.wifi.join = WJ_IDLE;
  m.wifi.joinSsid[0] = 0;
  t.tap(300, 66 + 92 + 3 * 76 + 20);
  t.tap(300, 66 + 2 * 76 + 38);  // 鈴木のiPhone
  check(viewState().wifiView == 2 && viewState().wifiLayer == 0 && !viewState().wifiHidden, "wifi: tap a locked network opens the keyboard (lower case, text shown)");
  shot("wifi_keys_lower", m);
  sameAsFull(m, "wifi_keys_lower: incremental == full");
  tapKey(t, TK_SHIFT);
  check(viewState().wifiLayer == 1, "wifi: ⇧ -> upper case");
  shot("wifi_keys_upper", m);
  sameAsFull(m, "wifi_keys_upper: incremental == full");
  tapKey(t, TK_LAYER);
  check(viewState().wifiLayer == 2, "wifi: #+= -> symbols");
  shot("wifi_keys_symbols", m);
  sameAsFull(m, "wifi_keys_symbols: incremental == full");
  tapKey(t, TK_LAYER);
  const bool backUpper = viewState().wifiLayer == 1;
  tapKey(t, TK_SHIFT);
  check(backUpper && viewState().wifiLayer == 0, "wifi: ABC -> upper again (⇧ stays), ⇧ -> lower");
  t.all.clear();
  bool typed = typeKeys(t, "abc1234");
  tapKey(t, TK_OK);
  check(typed && t.all.empty() && viewState().wifiView == 2, "wifi: 保存して接続 with 7 characters does nothing");
  typed = typeKeys(t, "5");
  shot("wifi_keys_typed_visible", m);
  sameAsFull(m, "wifi_keys_typed_visible: incremental == full");
  tapKey(t, TK_SHOW);
  check(viewState().wifiHidden, "wifi: 隠す hides the text");
  shot("wifi_keys_typed_hidden", m);
  sameAsFull(m, "wifi_keys_typed_hidden: incremental == full");
  tapKey(t, TK_SHOW);
  t.all.clear();
  tapKey(t, TK_OK);
  a = lastKind(t.all, ACT_WIFI_SAVE);
  check(typed && a && countKind(t.all, ACT_WIFI_SAVE) == 1 && t.all.size() == 1 && strcmp(a->ssid, "鈴木のiPhone") == 0 && strcmp(a->pass, "abc12345") == 0 && viewState().wifiView == 3,
        "wifi: abc12345 typed key by key, 保存して接続 -> exactly one ACT_WIFI_SAVE(鈴木のiPhone, abc12345)");
  // join results: IDLE (the app has not moved yet) looks like TRYING
  m.wifi.join = WJ_IDLE;
  frame(m, g_now, &d);
  const std::vector<uint16_t> idle(g_fb, g_fb + kWidth * kHeight);
  m.wifi.join = WJ_TRYING;
  copyStr(m.wifi.joinSsid, sizeof m.wifi.joinSsid, "鈴木のiPhone");
  shot("wifi_result_trying", m);
  check(screenIs(idle), "wifi: WJ_IDLE after the save shows 接続中… like WJ_TRYING");
  sameAsFull(m, "wifi_result_trying: incremental == full");
  m.wifi.join = WJ_OK;
  copyStr(m.wifi.joinSsid, sizeof m.wifi.joinSsid, "home-2G");  // a result for another network: still trying
  frame(m, g_now, &d);
  check(screenIs(idle), "wifi: a result for another network still shows 接続中…");
  copyStr(m.wifi.joinSsid, sizeof m.wifi.joinSsid, "鈴木のiPhone");
  m.wifi.join = WJ_BAD_PASSWORD;
  shot("wifi_result_bad_password", m);
  sameAsFull(m, "wifi_result_bad_password: incremental == full");
  t.tap(154, 398);  // 入力しなおす
  check(viewState().wifiView == 2, "wifi: 入力しなおす -> keyboard");
  sameAsFull(m, "wifi keyboard (入力しなおす): incremental == full");
  t.all.clear();
  tapKey(t, TK_OK);  // the previous text is back
  a = lastKind(t.all, ACT_WIFI_SAVE);
  check(a && strcmp(a->pass, "abc12345") == 0 && viewState().wifiView == 3, "wifi: 入力しなおす keeps the previous text (ACT_WIFI_SAVE abc12345 again)");
  m.wifi.join = WJ_FAILED;
  shot("wifi_result_failed", m);
  sameAsFull(m, "wifi_result_failed: incremental == full");
  m.wifi.join = WJ_NOT_FOUND;
  shot("wifi_result_not_found", m);
  sameAsFull(m, "wifi_result_not_found: incremental == full");
  m.wifi.join = WJ_OK;
  copyStr(m.wifi.ssid, sizeof m.wifi.ssid, "鈴木のiPhone");
  copyStr(m.wifi.ip, sizeof m.wifi.ip, "192.168.1.23");
  shot("wifi_result_ok", m);
  sameAsFull(m, "wifi_result_ok: incremental == full");
  t.tap(300, 398);  // OK
  check(viewState().wifiView == 0, "wifi: 接続しました: OK -> list");
  wifiSample(m);
  // hidden text: only dots, the same pixels for any 8 characters
  t.tap(300, 66 + 92 + 3 * 76 + 20);
  t.tap(300, 66 + 3 * 76 + 38);  // Buffalo-G-1A2B
  t.all.clear();
  tapKey(t, TK_OK);  // the keyboard starts empty (nothing kept from before)
  const bool empty = t.all.empty() && viewState().wifiView == 2;
  tapKey(t, TK_SHOW);
  typed = typeKeys(t, "abc12345");
  const std::vector<uint16_t> hidA = band();
  backspaces(t, 8);
  typed = typeKeys(t, "Zq#9 ~|x") && typed;
  const std::vector<uint16_t> hidB = band();
  tapKey(t, TK_SHOW);
  const std::vector<uint16_t> shown = band();
  check(empty && typed && hidA == hidB && hidA != shown, "wifi: hidden text is drawn as dots only (same pixels for abc12345 and Zq#9 ~|x)");
  // all 95 printable ASCII characters through the layers (a password holds at most 64: two passwords)
  backspaces(t, 8);
  std::string all1, all2;
  for (int c = 0x20; c < 0x50; c++) all1 += (char)c;
  for (int c = 0x50; c < 0x7F; c++) all2 += (char)c;
  t.all.clear();
  bool typedAll = typeKeys(t, all1.c_str());
  shot("wifi_keys_long", m);
  sameAsFull(m, "wifi_keys_long: incremental == full");
  tapKey(t, TK_OK);
  const Action *a1 = lastKind(t.all, ACT_WIFI_SAVE);
  const bool ok1 = a1 && all1 == a1->pass;
  m.wifi.join = WJ_BAD_PASSWORD;
  copyStr(m.wifi.joinSsid, sizeof m.wifi.joinSsid, "Buffalo-G-1A2B");
  frame(m, g_now, &d);
  t.tap(154, 398);  // 入力しなおす
  backspaces(t, (int)all1.size());
  t.all.clear();
  typedAll = typeKeys(t, all2.c_str()) && typedAll;
  tapKey(t, TK_OK);
  const Action *a2 = lastKind(t.all, ACT_WIFI_SAVE);
  const bool ok2 = a2 && all2 == a2->pass;
  snprintf(det, sizeof det, "0x20..0x4F %s, 0x50..0x7E %s", ok1 ? "ok" : "differs", ok2 ? "ok" : "differs");
  check(typedAll && ok1 && ok2, "wifi: all 95 printable ASCII characters typed through the layers (two passwords)", det);
  // 64 characters: only hex digits are valid
  t.tap(154, 398);
  backspaces(t, (int)all2.size());
  std::string hex64, bad64(63, 'a');
  for (int i = 0; i < 4; i++) hex64 += "0123456789abcdef";
  bad64 += 'g';
  t.all.clear();
  typed = typeKeys(t, bad64.c_str());
  tapKey(t, TK_OK);
  const bool badNone = t.all.empty();
  backspaces(t, 64);
  typed = typeKeys(t, hex64.c_str()) && typed;
  tapKey(t, TK_OK);
  a = lastKind(t.all, ACT_WIFI_SAVE);
  check(typed && badNone && a && hex64 == a->pass, "wifi: 64 characters: hex digits saved, other 64 not");
  // manual name, then its password
  m.wifi.join = WJ_OK;
  copyStr(m.wifi.joinSsid, sizeof m.wifi.joinSsid, "Buffalo-G-1A2B");
  frame(m, g_now, &d);
  t.tap(300, 398);  // OK -> list
  t.tap(300, 66 + 92 + 3 * 76 + 20);
  t.swipe(300, 380, 300, 80);
  t.swipe(300, 380, 300, 80);
  check(viewState().scroll == 7 * 76 - (398 - 66), "wifi: the scan list scrolls");
  t.tap(300, 66 + 6 * 76 + 38 - viewState().scroll);  // 名前を入力
  t.all.clear();
  typed = typeKeys(t, "MyNet 5G");
  shot("wifi_keys_name", m);
  sameAsFull(m, "wifi_keys_name: incremental == full");
  tapKey(t, TK_OK);  // 次へ
  const bool toPass = t.all.empty() && viewState().wifiView == 2;
  typed = typeKeys(t, "password1") && typed;
  tapKey(t, TK_OK);
  a = lastKind(t.all, ACT_WIFI_SAVE);
  check(typed && toPass && a && countKind(t.all, ACT_WIFI_SAVE) == 1 && strcmp(a->ssid, "MyNet 5G") == 0 && strcmp(a->pass, "password1") == 0,
        "wifi: 名前を入力: MyNet 5G, 次へ, password1 -> ACT_WIFI_SAVE(MyNet 5G, password1)");
  // キャンセル, ‹ 戻る, ‹ 設定; leaving the page resets it
  m.wifi.join = WJ_NOT_FOUND;
  copyStr(m.wifi.joinSsid, sizeof m.wifi.joinSsid, "MyNet 5G");
  frame(m, g_now, &d);
  t.tap(300, 398);  // OK -> list
  t.tap(300, 66 + 92 + 3 * 76 + 20);
  t.tap(300, 66 + 38);  // iPhone (registered: a new password)
  typeKeys(t, "secret99");
  tapKey(t, TK_CANCEL);
  const bool cancelled = viewState().wifiView == 1;
  t.tap(300, 66 + 38);
  t.all.clear();
  tapKey(t, TK_OK);
  const bool wiped = t.all.empty();
  tapKey(t, TK_CANCEL);
  t.tap(60, 32);  // ‹ 戻る
  const bool backList = viewState().wifiView == 0;
  t.tap(60, 32);  // ‹ 設定
  check(cancelled && wiped && backList && m.screen == SC_SETTINGS, "wifi: キャンセル -> scan (text gone), ‹ 戻る -> list, ‹ 設定 -> settings");
  m.screen = SC_WIFI;
  frame(m, g_now, &d);
  check(viewState().wifiView == 0 && viewState().wifiDialog == 0, "wifi: the page starts at the list again");
}

// ---------------------------------------------------------------- timings
static double msNow() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
static void timings() {
  Model &m = g_m;
  lapModel(m);
  Dirty d;
  frame(m, g_now, &d);
  // full redraw
  const int N = 40;
  double t0 = msNow();
  for (int i = 0; i < N; i++) {
    invalidate();
    frame(m, g_now, &d);
  }
  const double full = (msNow() - t0) / N;
  // hands only (20 Hz: +50 ms of lap time, rpm unchanged)
  t0 = msNow();
  uint32_t px = 0;
  for (int i = 0; i < 200; i++) {
    g_now += 50;
    m.circuit.lapMs += 50;
    m.circuit.ghostMs += 50;
    frame(m, g_now, &d);
    px += d.pixels();
  }
  const double hands = (msNow() - t0) / 200;
  const uint32_t handsPx = px / 200;
  // ring + hands + texts (25 Hz frames, rpm climbing 100 rpm per frame, marker following, delta changing)
  t0 = msNow();
  px = 0;
  double worst = 0;
  for (int i = 0; i < 200; i++) {
    const double a = msNow();
    g_now += 40;
    m.circuit.lapMs += 40;
    m.circuit.ghostMs += 40;
    m.rpm = (uint16_t)(4400 + (i % 19) * 100);
    m.peakMarkerRpm = m.rpm;
    m.peakTopRpm = m.rpm;
    m.circuit.deltaS = -0.57f + 0.01f * (float)(i % 7);
    frame(m, g_now, &d);
    px += d.pixels();
    const double b = msNow() - a;
    if (b > worst) worst = b;
  }
  const double mixed = (msNow() - t0) / 200;
  const uint32_t mixedPx = px / 200;
  // flash: background toggles every 83 ms
  m.rpm = 6400;
  m.flash = true;
  t0 = msNow();
  int toggles = 0;
  bool last = false;
  double flashSum = 0;
  for (int i = 0; i < 120; i++) {
    g_now += 21;
    m.circuit.lapMs += 21;
    const double a = msNow();
    frame(m, g_now, &d);
    const double b = msNow() - a;
    if (viewState().flashRed != last) {
      toggles++;
      flashSum += b;
      last = viewState().flashRed;
    }
  }
  m.flash = false;
  const double circuitFlash = toggles ? flashSum / toggles : 0.0;
  // street: rpm frames and flash phase changes (no layer: the screen is drawn again)
  baseModel(m);
  m.drive = DRIVE_STREET;
  m.street = Street{ true, 88, true, 62, true, 210 };
  frame(m, g_now, &d);
  t0 = msNow();
  for (int i = 0; i < 200; i++) {
    g_now += 40;
    m.rpm = (uint16_t)(3000 + (i % 30) * 100);
    m.speedKmh = 40.0f + (float)(i % 50);
    frame(m, g_now, &d);
  }
  const double street = (msNow() - t0) / 200;
  m.rpm = 6400;
  m.flash = true;
  toggles = 0;
  flashSum = 0;
  last = false;
  for (int i = 0; i < 120; i++) {
    g_now += 21;
    const double a = msNow();
    frame(m, g_now, &d);
    const double b = msNow() - a;
    if (viewState().flashRed != last) {
      toggles++;
      flashSum += b;
      last = viewState().flashRed;
    }
  }
  const double streetFlash = toggles ? flashSum / toggles : 0.0;
  m.flash = false;
  // laps page: one vertical drag step (body redraw)
  m.screen = SC_LAPS;
  frame(m, g_now, &d);
  ViewState v = viewState();
  t0 = msNow();
  for (int i = 0; i < 100; i++) {
    v.scroll = (int16_t)(i % 2 ? 300 : 290);
    testSetView(v);
    frame(m, g_now, &d);
  }
  const double laps = (msNow() - t0) / 100;
  printf("TIME circuit full redraw            %.3f ms/frame\n", full);
  printf("TIME circuit hands only (20 Hz)     %.3f ms/frame, %u px pushed/frame\n", hands, (unsigned)handsPx);
  printf("TIME circuit ring+hands+text (25 Hz) %.3f ms/frame avg, %.3f ms worst, %u px pushed/frame\n", mixed, worst, (unsigned)mixedPx);
  printf("TIME circuit flash phase change     %.3f ms/frame\n", circuitFlash);
  printf("TIME street rpm/speed frame (25 Hz)  %.3f ms/frame\n", street);
  printf("TIME street flash phase change      %.3f ms/frame (full redraw)\n", streetFlash);
  printf("TIME laps page redraw (scroll step) %.3f ms/frame (full redraw)\n", laps);
}

// ---------------------------------------------------------------- main
// ---------------------------------------------------------------- 走行記録 (settings > 走行記録)
// The values the app fills from core/odolog: 2026 with records from 9/10 (ODO 22,000) to today 11/8 (22,615)
static void odoSample(Model &m, uint8_t level) {
  OdoPage &o = m.odo;
  o = OdoPage{};
  o.storageOk = true;
  o.records = 1234;
  o.curOk = true;
  o.cur = 22615;
  o.sinceBootKm = 12;
  o.dateOk = true;
  o.level = level;
  o.odoKnown = true;
  o.canBack = true;
  o.canFwd = false;
  o.current = true;
  o.canFiner = level < OL_DAY;
  switch (level) {
    case OL_YEAR:
      o.first = OdoDate{ 2026, 1, 1, 4 };
      o.last = OdoDate{ 2026, 12, 31, 4 };
      o.odoStart = 22000, o.odoEnd = 22615, o.aboveS = 5025, o.canBack = false;
      break;
    case OL_QUARTER:
      o.first = OdoDate{ 2026, 10, 1, 4 };
      o.last = OdoDate{ 2026, 12, 31, 4 };
      o.odoStart = 22050, o.odoEnd = 22615, o.aboveS = 3600;
      break;
    case OL_MONTH:
      o.first = OdoDate{ 2026, 10, 1, 4 };
      o.last = OdoDate{ 2026, 10, 31, 6 };
      o.odoStart = 22050, o.odoEnd = 22340, o.aboveS = 1800, o.current = false, o.canFwd = true;
      break;
    case OL_WEEK:
      o.first = OdoDate{ 2026, 11, 8, 0 };
      o.last = OdoDate{ 2026, 11, 14, 6 };
      o.odoStart = 22520, o.odoEnd = 22615, o.aboveS = 754;
      break;
    default:
      o.first = o.last = OdoDate{ 2026, 11, 5, 4 };
      o.odoStart = o.odoEnd = 22520, o.aboveS = 0, o.current = false, o.canFwd = true;
      break;
  }
  o.km = o.odoEnd - o.odoStart;
}
static void odoPages(Model &m) {
  m.screen = SC_ODO;
  static const char *const kNames[OL_COUNT] = { "odo_year", "odo_quarter", "odo_month_past", "odo_week", "odo_day_no_drive" };
  for (int lv = 0; lv < OL_COUNT; lv++) {
    odoSample(m, (uint8_t)lv);
    shot(kNames[lv], m);
  }
  sameAsFull(m, "odo_day_no_drive: incremental == full");
  odoSample(m, OL_QUARTER);
  m.odo.first = OdoDate{ 2026, 1, 1, 4 };
  m.odo.last = OdoDate{ 2026, 3, 31, 2 };
  m.odo.odoKnown = false, m.odo.canBack = false, m.odo.canFwd = true, m.odo.current = false;
  shot("odo_no_record", m);
  odoSample(m, OL_YEAR);
  m.odo.dateOk = false;
  shot("odo_wait_time", m);
  m.odo.curOk = false;
  m.odo.storageOk = false;
  shot("odo_wait_odo_no_area", m);
  sameAsFull(m, "odo_wait_odo_no_area: incremental == full");
  odoSample(m, OL_YEAR);
  m.screen = SC_SETTINGS;
  shot("page_settings_odo_row", m);
}
static void odoTests() {
  Model &m = g_m;
  baseModel(m);
  odoSample(m, OL_YEAR);
  m.touchEnabled = true;
  Touch t;
  t.m = &m;
  Dirty d;
  m.screen = SC_SETTINGS;
  frame(m, g_now, &d);
  t.tap(300, 66 + 38);  // 走行記録 (first row)
  const Action *a = t.find(ACT_PAGE);
  check(a && a->arg == SC_ODO && m.screen == SC_ODO, "odo: settings 走行記録 row -> ACT_PAGE(SC_ODO)");
  t.tap(16 + 2 * 114 + 54, 78 + 26);  // 1 ヶ月 tab
  a = t.find(ACT_ODO);
  check(a && a->arg == OM_LEVEL && a->value == OL_MONTH, "odo: tap 1 ヶ月 -> ACT_ODO(OM_LEVEL, month)");
  t.tap(16 + 54, 78 + 26);  // the tab shown
  check(!t.got(ACT_ODO), "odo: tap on the level shown does nothing");
  t.tap(100, 378 + 32);  // ‹ 前 (1 年: nothing earlier)
  check(!t.got(ACT_ODO), "odo: ‹ 前 does nothing without an earlier period");
  odoSample(m, OL_MONTH);
  frame(m, g_now, &d);
  t.tap(100, 378 + 32);
  a = t.find(ACT_ODO);
  check(a && a->arg == OM_PREV, "odo: ‹ 前 -> ACT_ODO(OM_PREV)");
  t.tap(484, 378 + 32);
  a = t.find(ACT_ODO);
  check(a && a->arg == OM_NEXT, "odo: 次 › -> ACT_ODO(OM_NEXT)");
  t.swipe(200, 250, 360, 254);  // finger to the right: earlier
  a = t.find(ACT_ODO);
  check(a && a->arg == OM_PREV && m.screen == SC_ODO, "odo: swipe right -> earlier period (not another page)");
  t.swipe(400, 250, 240, 254);
  a = t.find(ACT_ODO);
  check(a && a->arg == OM_NEXT && m.screen == SC_ODO, "odo: swipe left -> later period");
  m.odo.dateOk = false;
  frame(m, g_now, &d);
  t.tap(16 + 3 * 114 + 54, 78 + 26);
  t.tap(100, 378 + 32);
  t.swipe(200, 250, 360, 254);
  check(!t.got(ACT_ODO), "odo: before the date is known the tabs, buttons and swipes do nothing");
  t.tap(40, 32);  // ‹ 設定
  check(m.screen == SC_SETTINGS, "odo: ‹ 設定 -> settings");
}

int main(int argc, char **argv) {
  if (argc > 1) g_out = argv[1];
  mkdir("out", 0755);
  mkdir(g_out.c_str(), 0755);
  begin(g_fb, g_scratch, g_layer);
  Model &m = g_m;

  // ---- boot self-test
  baseModel(m);
  m.screen = SC_SELFTEST;
  shot("selftest_all_pass", m);
  sameAsFull(m, "selftest: incremental == full");
  m.selftest.row[1].result = TR_FAIL;
  copyStr(m.selftest.row[1].value, sizeof m.selftest.row[1].value, "応答なし（カードを確認）");
  m.selftest.row[5].result = TR_WAIT;
  m.selftest.advanceS = -1;
  frame(m, g_now, nullptr);
  shot("selftest_fail", m);
  sameAsFull(m, "selftest_fail: incremental == full");

  // ---- circuit
  baseModel(m);
  m.rpm = 900;
  m.speedKmh = 0;
  m.peakVisible = false;
  m.screen = SC_DRIVE;
  m.drive = DRIVE_CIRCUIT;
  shot("circuit_idle", m);
  lapModel(m);
  shot("circuit_lap", m);
  sameAsFull(m, "circuit_lap: incremental == full");
  m.rpm = 6100;
  m.peakMarkerRpm = 6100;
  m.peakTopRpm = 6100;
  shot("circuit_red", m);
  // flash: the first frame with model.flash starts the red phase; 83 ms later black
  m.rpm = 6400;
  m.peakMarkerRpm = m.peakTopRpm = 6400;
  m.flash = true;
  shot("circuit_flash_on", m);
  check(viewState().flashRed, "flash: first phase is red");
  g_now += 83;
  shot("circuit_flash_off", m);
  check(!viewState().flashRed, "flash: 83 ms later black");
  g_now += 83;
  frame(m, g_now, nullptr);
  check(viewState().flashRed, "flash: 166 ms later red again (6 Hz)");
  {  // background colours of the two phases, day and night (35 %)
    const uint16_t red = 0xF8E5, redNight = 0x5842;  // rgb(0xFF1E2D), rgb(0x590A10)
    const bool dayRed = g_fb[5 * kWidth + 5] == red;
    m.nightDim = true;
    frame(m, g_now, nullptr);
    const bool nightRed = g_fb[5 * kWidth + 5] == redNight;
    g_now += 83;
    frame(m, g_now, nullptr);
    const bool nightBlack = g_fb[5 * kWidth + 5] == 0;
    char det[96];
    snprintf(det, sizeof det, "corner pixel day %04X night %04X", (unsigned)red, (unsigned)redNight);
    check(dayRed && nightRed && nightBlack, "flash: red #FF1E2D by day, dimmed to 35 % at night, black phase black", det);
    sameAsFull(m, "flash at night: incremental == full");
    m.nightDim = false;
  }
  m.flash = false;
  // pre-run checklist (2 pending) and READY
  baseModel(m);
  m.rpm = 950;
  m.speedKmh = 18;
  m.check.show = true;
  m.check.n = 4;
  m.check.item[0] = CheckItem{ CK_SPORT, true };
  m.check.item[1] = CheckItem{ CK_ESC_OFF, false };
  m.check.item[2] = CheckItem{ CK_OIL, false };
  m.check.item[3] = CheckItem{ CK_RACECHRONO, true };
  m.circuit.lastOk = true;
  m.circuit.lastMs = 93980;
  m.circuit.bestOk = true;
  m.circuit.bestMs = 93320;
  shot("circuit_prerun", m);
  m.check.item[1].ok = m.check.item[2].ok = true;
  shot("circuit_ready", m);
  // drive sequence and the incremental == full check (with the layer, then without)
  lapModel(m);
  frame(m, g_now, nullptr);
  driveSequence(m, 150);
  sameAsFull(m, "circuit drive sequence (layer): incremental == full");
  memcpy(g_fb2, g_fb, sizeof g_fb);
  {
    static uint16_t fbA[kWidth * kHeight];
    memcpy(fbA, g_fb, sizeof fbA);
    begin(g_fb, g_scratch, nullptr);  // no layer: redraw-under-hands path
    frame(m, g_now, nullptr);
    int diff = 0;
    for (int i = 0; i < kWidth * kHeight; i++) diff += g_fb[i] != fbA[i];
    check(diff == 0, "circuit without layer == with layer (full)");
    lapModel(m);
    frame(m, g_now, nullptr);
    driveSequence(m, 80);
    sameAsFull(m, "circuit drive sequence (no layer): incremental == full");
    begin(g_fb, g_scratch, g_layer);
  }

  // ---- street
  baseModel(m);
  m.drive = DRIVE_STREET;
  m.rpm = 3200;
  m.speedKmh = 68;
  m.peakVisible = true;
  m.peakMarkerRpm = 5800;
  m.peakTopRpm = 5800;
  m.street = Street{ true, 88, true, 62, true, 210 };
  shot("street", m);
  sameAsFull(m, "street: incremental == full");
  m.oil = OIL_COLD;
  m.street.waterC = 45;
  m.peakVisible = false;
  m.rpm = 1100;
  m.speedKmh = 23;
  shot("street_cold_oil", m);
  sameAsFull(m, "street_cold_oil: incremental == full");
  for (int i = 0; i < 60; i++) {  // street sequence with a flash
    g_now += 40;
    m.rpm = (uint16_t)(3000 + i * 60);
    m.flash = m.rpm >= m.shift.flash;
    m.peakVisible = m.rpm > 3000;
    m.peakMarkerRpm = m.peakTopRpm = m.rpm;
    m.speedKmh = 30.0f + (float)i;
    frame(m, g_now, nullptr);
  }
  sameAsFull(m, "street sequence: incremental == full");
  m.flash = false;

  // ---- pages
  baseModel(m);
  m.touchEnabled = true;
  m.screen = SC_LAPS;
  shot("page_laps", m);
  {
    ViewState v = view(SC_LAPS);
    v.scroll = 76 * 4;
    v.lapOpen = 7;  // the best lap (L8)
    shot("page_laps_sectors", m, &v);
    sameAsFull(m, "page_laps_sectors: incremental == full");
  }
  m.screen = SC_MAINT;
  shot("page_maint", m);
  {
    ViewState v = view(SC_MAINT);
    v.maintDialog = 0;
    shot("page_maint_dialog", m, &v);
  }
  m.screen = SC_DATA;
  shot("page_data_connected", m);
  m.data.wifiOk = false;
  m.data.ip[0] = 0;
  shot("page_data_offline", m);
  m.data.nNetworks = 0;
  shot("page_data_offline_unregistered", m);
  sameAsFull(m, "page_data_offline_unregistered: incremental == full");
  m.data.nNetworks = 2;
  m.data.wifiOk = true;
  copyStr(m.data.ip, sizeof m.data.ip, "172.20.10.3");
  m.screen = SC_SETTINGS;
  shot("page_settings", m);
  {
    ViewState v = view(SC_SETTINGS);
    v.editor = 3;  // シフトポイント (after 走行記録, コース, ゲート設定)
    v.shiftSel = 2;
    v.scroll = 152;
    shot("page_settings_shift_edit", m, &v);
  }
  m.screen = SC_GATE;
  {
    ViewState v = view(SC_GATE);
    v.gatePick = 300;
    shot("page_gate", m, &v);
    sameAsFull(m, "page_gate: incremental == full");
  }

  odoPages(m);

  // ---- touch, Wi-Fi page, 走行記録, glyphs, timings
  touchTests();
  wifiTests();
  odoTests();
  {
    char det[64];
    snprintf(det, sizeof det, "%u missing", (unsigned)missingGlyphs());
    check(missingGlyphs() == 0, "fonts: no missing glyphs in any scenario", missingGlyphs() ? det : "");
  }
  timings();
  printf("PNG %d files in %s/\n", (int)g_pngs.size(), g_out.c_str());
  printf("RESULT: %s (%d PASS, %d FAIL)\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
