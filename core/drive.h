// core/drive.h - what the car is doing, as the gadget needs it (UI設計 §2, §6, §9; 計画 要件 11, 12).
// DriveState::update() is called at every GNSS epoch and at least every 100 ms when there is none
// (a timer), with the speeds and the course flags. It decides:
//  - touch: enabled after < 10 km/h for 2 s (or when stopped), disabled at once above 15 km/h
//  - stopped: < 2 km/h (or no speed at all) for 30 s -> the session closes, Wi-Fi is allowed
//  - Wi-Fi: allowed from "stopped" until the speed goes above 15 km/h
//  - session: opens when faster than 10 km/h for 3 s, closes when stopped
//  - screen: circuit when within 3 km of a course or while a session with a course is open, else street
//  - pre-run checklist (UI設計 §6): near a course, not run yet, shown below 30 km/h and hidden above
//    60 km/h (between the two it stays as it was). "Run" = a lap has started in the session (the lap
//    timer crossed S/F); it is forgotten when the car leaves the course area or the session closes,
//    so the panel comes back before the next run.
//  - track time: in a course area and faster than 20 km/h; odometer: integral of the speed
// Speed used for all of it: CAN vehicle speed, the GNSS speed when CAN has none.
// Also here: checklist.json (which checklist items are used) and wifi.json (networks to join).
#pragma once

#include <cstdint>

#include "json.h"

namespace lt {

constexpr float kTouchOnKmh = 10.0f;        // touch enabled after slower than this ...
constexpr uint32_t kTouchOnMs = 2000;       // ... for this long
constexpr float kTouchOffKmh = 15.0f;       // touch disabled and Wi-Fi off above this
constexpr float kStopKmh = 2.0f;            // "speed 0"
constexpr uint32_t kStopMs = 30000;         // that long: stopped (session closes, Wi-Fi allowed)
constexpr float kMoveKmh = 10.0f;           // moving: faster than this ...
constexpr uint32_t kMoveMs = 3000;          // ... for this long: the session opens
constexpr float kChecklistShowKmh = 30.0f;  // pre-run checklist shown below this ...
constexpr float kChecklistHideKmh = 60.0f;  // ... and hidden above this
constexpr float kTrackTimeKmh = 20.0f;      // track time counts above this, in a course area
constexpr uint32_t kMaxStepMs = 1000;       // a longer gap between updates adds no distance/track time

enum class Screen : uint8_t { Street, Circuit };
enum class SpeedSource : uint8_t { None, Can, Gnss };

struct DriveInput {
  uint32_t nowMs = 0;          // millis()
  bool canSpeedOk = false;     // vehicle speed from CAN
  float canSpeedKmh = 0.0f;
  bool gnssSpeedOk = false;    // GNSS ground speed of a usable fix
  float gnssSpeedKmh = 0.0f;
  bool nearCourse = false;     // CourseDb::nearest(...) >= 0: within 3 km of a course centre
  bool courseSession = false;  // the open session has a course (its cid is not "street")
  bool lapRunning = false;     // LapTimer::timing() of the current session (reset it with the session)
};

// Events returned by update() (state changes of this call)
constexpr uint32_t kDrvTouchOn = 1u << 0;
constexpr uint32_t kDrvTouchOff = 1u << 1;
constexpr uint32_t kDrvSessionOpen = 1u << 2;   // start recording a session
constexpr uint32_t kDrvSessionClose = 1u << 3;  // close it
constexpr uint32_t kDrvWifiOn = 1u << 4;        // Wi-Fi may connect from now
constexpr uint32_t kDrvWifiOff = 1u << 5;       // disconnect Wi-Fi
constexpr uint32_t kDrvStopped = 1u << 6;       // became stopped
constexpr uint32_t kDrvScreen = 1u << 7;        // screen() changed
constexpr uint32_t kDrvChecklist = 1u << 8;     // checklistVisible() changed

class DriveState {
 public:
  DriveState() { reset(); }
  void reset();  // power-on state: touch enabled, no session, Wi-Fi not allowed, street screen
  uint32_t update(const DriveInput& in);

  bool speedOk() const { return src_ != SpeedSource::None; }
  float speedKmh() const { return kmh_; }
  SpeedSource speedSource() const { return src_; }
  bool touchEnabled() const { return touch_; }
  bool stopped() const { return stopped_; }
  bool sessionOpen() const { return session_; }
  bool wifiAllowed() const { return wifi_; }
  Screen screen() const { return screen_; }
  bool checklistVisible() const { return checklist_; }
  bool hasRun() const { return hasRun_; }

  // Odometer = the value set from the settings + the integrated speed. Keep odometerMm() and
  // trackMs() in NVS and give them back after power-on.
  void setOdometerKm(float km);
  float odometerKm() const { return static_cast<float>(odoMm_) * 1e-6f; }
  int64_t odometerMm() const { return odoMm_; }
  void setOdometerMm(int64_t mm) {
    odoMm_ = mm;
    odoFracMm_ = 0.0f;
  }
  float trackHours() const { return static_cast<float>(trackMs_) / 3600000.0f; }
  int64_t trackMs() const { return trackMs_; }
  void setTrackMs(int64_t ms) { trackMs_ = ms; }

 private:
  static uint32_t held(bool cond, bool* on, uint32_t* since, uint32_t now);

  bool started_;
  uint32_t lastMs_;
  SpeedSource src_;
  float kmh_;
  bool prevKnown_;
  float prevKmh_;
  bool slowOn_, stillOn_, moveOn_;
  uint32_t slowSince_, stillSince_, moveSince_;
  bool touch_, stopped_, session_, wifi_, hasRun_, slowForList_, checklist_;
  Screen screen_;
  int64_t odoMm_ = 0;
  float odoFracMm_ = 0.0f;
  int64_t trackMs_ = 0;
};

// ---- checklist.json: {"version":1,"sport":true,"esc_off":true,"oil_ok":true,"racechrono":false} ----
// true = the item is used. Missing keys keep the defaults below.
struct ChecklistConfig {
  bool sport = true;        // スポーツモード ON
  bool escOff = true;       // ESC / ASR OFF
  bool oilOk = true;        // 油温 OK
  bool racechrono = false;  // RaceChrono 接続 (BLE subscribed)
};
// Codes: JSON syntax, bad-version, missing-version, expected-bool. On failure *out keeps the defaults.
bool loadChecklist(json::Source& src, ChecklistConfig* out, json::Error* err);
bool loadChecklist(const char* text, int len, ChecklistConfig* out, json::Error* err);
int checklistJson(const ChecklistConfig& c, char* out, int cap);

enum ChecklistItem { kChkSport, kChkEscOff, kChkOilOk, kChkRaceChrono, kChkCount };

struct ChecklistSignals {  // *Ok = the signal is known (VehDecoder); not known: the item is not shown
  bool sportOk = false;
  bool sport = false;
  bool escOk = false;
  bool escOff = false;
  bool oilOk = false;
  bool oilWarm = false;
  bool bleSubscribed = false;
};

struct ChecklistView {
  bool shown[kChkCount];  // used in checklist.json and its signal is known
  bool done[kChkCount];
  int numShown;
  int open;    // shown and not done: the "未完了 n" badge
  bool ready;  // something shown and all done: "READY"
};
ChecklistView evaluateChecklist(const ChecklistConfig& cfg, const ChecklistSignals& sig);

// ---- wifi.json: {"version":1,"hostname":"laptimer","networks":[{"ssid":"..","pass":".."}]} -------
// Order = priority: join the first network that is visible.
constexpr int kWifiMaxNetworks = 8;
struct WifiNetwork {
  char ssid[33];  // 1..32 bytes
  char pass[65];  // "" (open), 8..63 characters, or 64 hex digits
};
struct WifiConfig {
  char hostname[32];  // [a-z0-9-], 1..31 (missing: "laptimer")
  int numNetworks;
  WifiNetwork networks[kWifiMaxNetworks];
};
// Codes: JSON syntax, bad-version, missing-version, bad-hostname, missing-networks,
// too-many-networks, missing-ssid, bad-ssid, bad-pass. On failure *out has no networks.
bool loadWifiConfig(json::Source& src, WifiConfig* out, json::Error* err);
bool loadWifiConfig(const char* text, int len, WifiConfig* out, json::Error* err);

}  // namespace lt
