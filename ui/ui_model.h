// ui/ui_model.h - everything the screens show (ui::Model), touch input (ui::Input) and user actions (ui::Action).
//
// The app owns the Model and fills it every loop; the UI only reads it. The UI never decides what the car is
// doing: the app chooses the screen (self-test, the drive screen as circuit or street, the stopped pages), whether
// touch is enabled, the peak hold values (core/peakhold), the flash, the ghost delta, the checklist visibility.
// The UI keeps only its own view state (scroll positions, an open dialog or editor, the gate point picked on the
// track) and resets it when model.screen changes.
// Strings are UTF-8, NUL-terminated, fixed size (no heap); fill them with ui::copyText() (cuts at a character
// boundary, a cut-off character would show as a box). Japanese text from the configuration (maintenance item
// names, course names, Wi-Fi names) is drawn with the 18/22/28 px fonts, which hold kana and common kanji; a
// character that is not in the font data is drawn as a box (ui::missingGlyphs() counts them).
#pragma once
#include <stdint.h>

namespace ui {

constexpr int kWidth = 600;
constexpr int kHeight = 450;

// ---------------------------------------------------------------- what is on the screen (chosen by the app)
enum Screen : uint8_t {
  SC_SELFTEST = 0,  // boot self-test (UI設計 §3)
  SC_DRIVE,         // driving screen: circuit chronograph or street (Model::drive)
  SC_LAPS,          // stopped pages, swipe order: SC_DRIVE <-> SC_LAPS <-> SC_MAINT <-> SC_DATA <-> SC_SETTINGS
  SC_MAINT,
  SC_DATA,
  SC_SETTINGS,
  SC_GATE,          // settings > gate setting (child page of SC_SETTINGS)
  SC_WIFI,          // settings > Wi-Fi: registered networks, scan, on-screen keyboard (child page of SC_SETTINGS)
  SC_COUNT
};
enum DriveKind : uint8_t { DRIVE_CIRCUIT = 0, DRIVE_STREET };

// Small status dots and pills
enum Status : uint8_t { ST_OFF = 0, ST_OK, ST_WARN, ST_ERR };  // grey, green, yellow, red
enum OilState : uint8_t { OIL_UNKNOWN = 0, OIL_OK, OIL_COLD };  // unknown = not shown (switch not identified)

struct ShiftPoints {  // rpm thresholds (UI設計 §4.1); the ring and the bar cover green .. green + 2000
  uint16_t green = 4500;
  uint16_t yellow = 5500;
  uint16_t red = 6000;
  uint16_t flash = 6300;
};

struct Clock {  // local time
  bool ok = false;
  uint16_t year = 0;
  uint8_t month = 0, day = 0, hour = 0, minute = 0, second = 0;
};

// ---------------------------------------------------------------- boot self-test (§3)
enum TestResult : uint8_t { TR_WAIT = 0, TR_PASS, TR_FAIL };
struct SelfTestRow {
  char label[8];   // "GNSS", "XTSD", "CAN", "IMU", "TOUCH", "BLE", "WiFi", "5V"
  char value[96];  // e.g. "25.0 Hz ・ 14 衛星 ・ hAcc 0.6 m" (shrinks to 18 px, then cut with "…" when too long)
  uint8_t result;  // TestResult
};
struct SelfTest {
  uint8_t n = 0;  // rows used (8)
  SelfTestRow row[8];
  int8_t advanceS = -1;  // seconds until the app goes to the drive screen (all PASS); -1 = no countdown shown
};

// ---------------------------------------------------------------- circuit chronograph (§4)
struct Circuit {
  uint16_t lap = 0;          // current lap number; 0 = before the first S/F crossing
  uint8_t sector = 0;        // current sector (1-based); 0 = none
  bool running = false;      // S/F crossed: lap time runs (false: --:--.-- grey, hands at 0, no ghost)
  uint32_t lapMs = 0;        // elapsed in the current lap (the app updates it every frame)
  bool lastOk = false;
  uint32_t lastMs = 0;
  bool bestOk = false;
  uint32_t bestMs = 0;
  bool ghostOk = false;      // ghost hands (best lap at the same place)
  uint32_t ghostMs = 0;
  bool deltaOk = false;      // VS BEST; false: "--"
  float deltaS = 0.0f;       // current - ghost [s]; negative = faster (green)
};

enum CheckId : uint8_t { CK_SPORT = 0, CK_ESC_OFF, CK_OIL, CK_RACECHRONO };
struct CheckItem {
  uint8_t id;  // CheckId (label: スポーツ ON / ESC / ASR OFF / 油温 OK / RaceChrono)
  bool ok;
};
struct Checklist {  // pre-run check (§6); only the items the app shows (unidentified CAN bits are left out)
  bool show = false;
  uint8_t n = 0;
  CheckItem item[4];
};

// ---------------------------------------------------------------- street (§5)
struct Street {
  bool waterOk = false;
  float waterC = 0.0f;
  bool fuelOk = false;
  float fuelPct = 0.0f;
  bool rangeOk = false;
  uint16_t rangeKm = 0;
};

// ---------------------------------------------------------------- maintenance (§5, §7)
enum MaintState : uint8_t { MS_OK = 0, MS_SOON, MS_OVERDUE };  // green, yellow (<= 20 %), red
struct MaintItem {
  char id[24];      // maint.json id (lt::MaintItem id: up to 23 characters; returned in ACT_MAINT_DONE)
  char name[48];    // e.g. "エンジンオイル"
  char remain[40];  // e.g. "残り 320 km", "期限超過 12 日", "サーキット 残り 3.1 h"
  float fraction;   // remaining part of the interval 0..1 (bar)
  uint8_t state;    // MaintState
};
struct Maint {  // in the order to show (most urgent first)
  uint8_t n = 0;
  MaintItem item[12];
  bool odoOk = false;
  uint32_t odoKm = 0;
  float trackH = 0.0f;  // hours driven on registered courses
};

// ---------------------------------------------------------------- laps page (§7)
struct LapRow {
  uint16_t no;      // lap number
  uint32_t ms;      // lap time
  bool valid;       // counts for BEST
  uint8_t nSec;     // sector times available (0..4)
  uint32_t secMs[4];
};
struct Laps {
  uint8_t n = 0;  // rows in driving order
  LapRow lap[64];
  char course[64] = "";  // header: course, start time, lap count
  bool startOk = false;
  uint8_t month = 0, day = 0, hour = 0, minute = 0;
};

// ---------------------------------------------------------------- data page (§7)
struct SessionCard {
  uint8_t month, day, hour, minute;
  char course[64];  // "" = street
  uint16_t laps;
  bool bestOk;
  uint32_t bestMs;
  bool downloaded;  // ダウンロード済み (green) / 未ダウンロード (grey)
};
struct DataPage {
  char hostname[24] = "laptimer";  // shown as <hostname>.local
  bool wifiOk = false;             // connected
  char ip[16] = "";
  char ssid[33] = "";
  int16_t remainHours = -1;  // recording time left on the XTSD; -1 = unknown
  uint16_t undownloaded = 0;
  uint8_t nSessions = 0;  // newest first
  SessionCard session[16];
  uint8_t nNetworks = 0;  // wifi.json networks in priority order (shown while not connected)
  char network[4][33];
};

// ---------------------------------------------------------------- settings page (§7)
struct SettingsPage {
  uint8_t brightness = 80;  // [%]
  bool courseAuto = true;   // course chosen automatically (3 km rule)
  int8_t courseIndex = -1;  // fixed course: index into courses[]
  uint8_t nCourses = 0;
  char courses[8][64];
  bool gateSet[3] = { false, false, false };  // S/F, S1, S2 defined for the current course
  bool oilInvert = false;   // oil temperature switch polarity inverted
  bool raceChrono = false;  // RaceChrono is part of the pre-run check
  bool recording = true;    // logging to the XTSD
};

// ---------------------------------------------------------------- gate setting (§7)
struct GatePoint {
  int32_t latE7, lonE7;
};
struct GateMark {
  bool set;
  int32_t latE7, lonE7;
  float headingDeg;  // direction of travel (0 = north, clockwise)
  float widthM;
};
struct GatePage {
  uint16_t n = 0;  // track of the previous session, in driving order (the app thins it out)
  GatePoint pt[600];
  GateMark gate[3];  // S/F, S1, S2
  char course[64] = "";
  bool startOk = false;
  uint8_t month = 0, day = 0, hour = 0, minute = 0;
};

// ---------------------------------------------------------------- Wi-Fi (settings > Wi-Fi, SC_WIFI)
// The networks are registered on this page (the password is typed on the on-screen keyboard) and kept in the
// device (NVS). The typed text stays inside the UI until ACT_WIFI_SAVE; the model never holds a password.
enum WifiJoin : uint8_t {
  WJ_IDLE = 0,      // nothing asked
  WJ_TRYING,        // joining joinSsid
  WJ_OK,            // joined joinSsid (connected / ssid / ip below)
  WJ_NOT_FOUND,     // joinSsid not seen in the scan
  WJ_BAD_PASSWORD,  // refused by the access point (handshake / authentication failure)
  WJ_FAILED,        // other failure (timeout, no address)
};
struct WifiFound {
  char ssid[33];  // UTF-8, may contain characters the fonts do not have (drawn as boxes)
  int8_t rssi;    // dBm
  bool locked;    // needs a password
  bool saved;     // already registered
};
struct WifiPage {
  uint8_t nSaved = 0;
  char saved[8][33];       // registered networks, in the order they are tried (first = first tried)
  bool scanning = false;   // a scan is running (after ACT_WIFI_SCAN)
  bool scanOk = false;     // found[] holds the result of the last finished scan
  uint8_t nFound = 0;
  WifiFound found[16];     // strongest first, hidden networks (empty name) left out
  bool connected = false;  // joined a network (any of the registered ones)
  char ssid[33] = "";      // the joined network
  char ip[16] = "";
  uint8_t join = WJ_IDLE;  // WifiJoin: result for joinSsid after ACT_WIFI_SAVE
  char joinSsid[33] = "";
};

// ---------------------------------------------------------------- the whole model
struct Model {
  uint8_t screen = SC_SELFTEST;  // Screen, chosen by the app
  uint8_t drive = DRIVE_STREET;  // DriveKind for SC_DRIVE
  bool touchEnabled = false;     // false: ui::touch() ignores everything
  bool nightDim = false;         // night: the shift flash is dimmed to 35 %

  // engine (CAN). rpmOk false after 0.5 s without RPM -> ring/bar dark
  bool rpmOk = false;
  uint16_t rpm = 0;
  bool flash = false;  // shift flash active (rpm >= shift.flash); the UI times the 6 Hz phase
  ShiftPoints shift;
  // peak hold (core/peakhold): triangles at markerRpm, centre number = topRpm, both only while visible
  bool peakVisible = false;
  uint16_t peakMarkerRpm = 0;
  uint16_t peakTopRpm = 0;

  // position (GNSS)
  bool speedOk = false;
  float speedKmh = 0.0f;
  bool gpsOk = false;
  float hAccM = 0.0f;
  uint8_t sats = 0;

  // status pills / dots
  uint8_t stLog = ST_OFF, stCan = ST_OFF, stBle = ST_OFF, stWifi = ST_OFF;  // Status
  uint8_t oil = OIL_UNKNOWN;                                                  // OilState

  Clock clock;
  char fwVersion[12] = "0.1.0";
  char courseName[64] = "";  // current course (circuit), "" = none

  SelfTest selftest;
  Circuit circuit;
  Checklist check;
  Street street;
  Maint maint;
  Laps laps;
  DataPage data;
  SettingsPage settings;
  GatePage gate;
  WifiPage wifi;
};

// ---------------------------------------------------------------- touch input (600x450 landscape coordinates)
enum InputKind : uint8_t { IN_DOWN = 0, IN_MOVE, IN_UP };
struct Input {
  uint8_t kind;  // InputKind
  int16_t x, y;  // IN_UP may repeat the last position
  uint32_t ms;   // time of the event
};

// ---------------------------------------------------------------- actions (UI -> app)
enum ActionKind : uint8_t {
  ACT_NONE = 0,
  ACT_PAGE,           // arg = Screen to show (swipe, ‹ 設定, ゲート設定). The app sets model.screen (or not).
  ACT_SELFTEST_SKIP,  // tap on the self-test screen
  ACT_MAINT_DONE,     // 実施済みにする: id = MaintItem::id, arg = index in model.maint
  ACT_GATE_SET,       // arg = 0 S/F, 1 S1, 2 S2; latE7/lonE7/headingDeg of the picked track point, value = width [m]
  ACT_SETTING,        // arg = SettingKey, value
  ACT_RECORD_STOP,    // 記録 (settings) tapped while recording
  ACT_RECORD_RESUME,  // ... while stopped
  ACT_WIFI_SCAN,      // Wi-Fi page: search for networks (model.wifi.scanning, then found[])
  ACT_WIFI_SAVE,      // register ssid with pass ("" = open network; else 8..63 characters or 64 hex digits),
                      // replacing the password of a registered one; it goes first in the list and is joined now
  ACT_WIFI_FORGET,    // remove the registered network ssid
};
enum SettingKey : uint8_t {
  SET_SHIFT_GREEN = 0,  // value [rpm]
  SET_SHIFT_YELLOW,
  SET_SHIFT_RED,
  SET_SHIFT_FLASH,
  SET_BRIGHTNESS,  // value [%] 10..100
  SET_COURSE,      // value = index into settings.courses, -1 = automatic
  SET_OIL_INVERT,  // value 0/1
  SET_RACECHRONO,  // value 0/1
};
struct Action {
  uint8_t kind = ACT_NONE;  // ActionKind
  uint8_t arg = 0;
  int32_t value = 0;
  int32_t latE7 = 0, lonE7 = 0;
  float headingDeg = 0.0f;
  char id[24] = "";
  char ssid[33] = "";  // ACT_WIFI_SAVE / ACT_WIFI_FORGET
  char pass[65] = "";  // ACT_WIFI_SAVE
};
struct ActionQueue {
  static constexpr int kMax = 8;
  uint8_t n = 0;
  Action a[kMax];
  bool push(const Action &x) {
    if (n >= kMax) return false;
    a[n++] = x;
    return true;
  }
  void clear() { n = 0; }
};

// ---------------------------------------------------------------- dirty rectangles (UI -> display driver)
struct Rect {
  int16_t x0, y0, x1, y1;  // x1, y1 exclusive
};
struct Dirty {
  static constexpr int kMax = 48;
  uint8_t n = 0;
  Rect r[kMax];
  uint32_t pixels() const {
    uint32_t s = 0;
    for (int i = 0; i < n; i++) s += (uint32_t)(r[i].x1 - r[i].x0) * (uint32_t)(r[i].y1 - r[i].y0);
    return s;
  }
};

}  // namespace ui
