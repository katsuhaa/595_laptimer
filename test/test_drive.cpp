// core/drive: touch / session / Wi-Fi / screen / pre-run checklist rules, odometer and track time,
// checklist.json and wifi.json.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/drive.h"
#include "core/json.h"
#include "tmini.h"

namespace {

std::string readFile(const char* path) {
  std::string s;
  FILE* f = std::fopen(path, "rb");
  if (!f) return s;
  char b[4096];
  size_t n = 0;
  while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
  std::fclose(f);
  return s;
}

// 25 Hz updates with the CAN speed (or none)
struct Sim {
  lt::DriveState ds;
  lt::DriveInput in;
  uint32_t t = 1000;
  uint32_t step(float kmh, bool known = true) {
    t += 40;
    in.nowMs = t;
    in.canSpeedOk = known;
    in.canSpeedKmh = kmh;
    return ds.update(in);
  }
  // `ms` more of the same speed; the events of all steps
  uint32_t run(uint32_t ms, float kmh, bool known = true) {
    uint32_t ev = 0;
    for (uint32_t e = 0; e < ms; e += 40) ev |= step(kmh, known);
    return ev;
  }
};

}  // namespace

TEST(drive_touch_hysteresis_10_15_kmh_2s) {
  Sim s;
  CHECK((s.run(1000, 0.0f) & (lt::kDrvTouchOn | lt::kDrvTouchOff)) == 0);
  CHECK(s.ds.touchEnabled());  // power-on: standing, touch on
  CHECK(s.step(15.1f) & lt::kDrvTouchOff);  // above 15 km/h: off at once
  CHECK(!s.ds.touchEnabled());
  s.run(5000, 12.0f);  // between 10 and 15: stays off
  CHECK(!s.ds.touchEnabled());
  s.step(9.9f);         // slower than 10 from here
  s.run(1960, 9.9f);    // 1.96 s
  CHECK(!s.ds.touchEnabled());
  CHECK(s.step(9.9f) & lt::kDrvTouchOn);  // 2.00 s
  CHECK(s.ds.touchEnabled());
  s.run(5000, 14.9f);  // stays on up to 15
  CHECK(s.ds.touchEnabled());
  s.step(9.0f);
  s.run(1000, 9.0f);
  CHECK(s.step(16.0f) & lt::kDrvTouchOff);
  // 1 s slow, a fast sample, 1.5 s slow: the 2 s start again
  s.run(1000, 5.0f);
  s.step(12.0f);
  s.run(1500, 5.0f);
  CHECK(!s.ds.touchEnabled());
  s.run(600, 5.0f);
  CHECK(s.ds.touchEnabled());
}

TEST(drive_stop_30s_session_and_wifi) {
  Sim s;
  s.step(0.0f);  // still from here
  CHECK((s.run(29960, 0.0f) & lt::kDrvStopped) == 0);
  CHECK(!s.ds.stopped() && !s.ds.wifiAllowed() && !s.ds.sessionOpen());
  uint32_t ev = s.step(0.0f);  // 30.00 s
  CHECK((ev & lt::kDrvStopped) && (ev & lt::kDrvWifiOn) && !(ev & lt::kDrvSessionClose));
  CHECK(s.ds.stopped() && s.ds.wifiAllowed());
  // moving at 12 km/h: the session opens after 3 s, Wi-Fi stays allowed below 15 km/h
  ev = s.step(12.0f);
  CHECK(!s.ds.stopped() && s.ds.wifiAllowed());
  ev |= s.run(2960, 12.0f);
  CHECK(!(ev & lt::kDrvSessionOpen) && !s.ds.sessionOpen());
  CHECK(s.step(12.0f) & lt::kDrvSessionOpen);
  CHECK(s.ds.sessionOpen());
  ev = s.step(15.5f);
  CHECK((ev & lt::kDrvWifiOff) && (ev & lt::kDrvTouchOff) && !s.ds.wifiAllowed());
  s.run(60000, 80.0f);
  // stop-and-go shorter than 30 s keeps the session
  s.run(20000, 0.0f);
  s.run(2000, 5.0f);
  s.run(20000, 1.0f);
  CHECK(s.ds.sessionOpen() && !s.ds.wifiAllowed() && s.ds.touchEnabled());
  ev = s.run(10040, 1.0f);  // 30.00 s below 2 km/h
  CHECK((ev & lt::kDrvSessionClose) && (ev & lt::kDrvWifiOn) && (ev & lt::kDrvStopped));
  CHECK(!s.ds.sessionOpen() && s.ds.wifiAllowed() && s.ds.stopped());
  // crawling at 5 km/h: not stopped any more, but no new session either (not > 10 km/h)
  s.run(10000, 5.0f);
  CHECK(!s.ds.stopped() && !s.ds.sessionOpen() && s.ds.wifiAllowed());
}

TEST(drive_no_speed_counts_as_standing) {
  Sim s;
  s.run(5000, 50.0f);
  CHECK(s.ds.sessionOpen() && !s.ds.touchEnabled());
  // no CAN and no GNSS (e.g. a garage without a fix): stopped after 30 s
  s.run(20000, 0.0f, false);
  CHECK(s.ds.sessionOpen() && !s.ds.speedOk() && !s.ds.touchEnabled());
  const uint32_t ev = s.run(10040, 0.0f, false);
  CHECK((ev & lt::kDrvSessionClose) && (ev & lt::kDrvTouchOn) && (ev & lt::kDrvWifiOn));
}

TEST(drive_speed_source_can_then_gnss) {
  lt::DriveState ds;
  lt::DriveInput in;
  in.nowMs = 100;
  in.canSpeedOk = true;
  in.canSpeedKmh = 0.0f;
  in.gnssSpeedOk = true;
  in.gnssSpeedKmh = 50.0f;
  ds.update(in);
  CHECK(ds.speedSource() == lt::SpeedSource::Can && ds.speedKmh() == 0.0f);
  in.canSpeedOk = false;
  in.nowMs += 40;
  ds.update(in);
  CHECK(ds.speedSource() == lt::SpeedSource::Gnss && ds.speedKmh() == 50.0f);
  in.canSpeedOk = true;
  in.canSpeedKmh = NAN;
  in.nowMs += 40;
  ds.update(in);
  CHECK(ds.speedSource() == lt::SpeedSource::Gnss);
  in.canSpeedOk = false;
  in.gnssSpeedOk = false;
  in.nowMs += 40;
  ds.update(in);
  CHECK(ds.speedSource() == lt::SpeedSource::None && !ds.speedOk());
}

TEST(drive_screen_and_pre_run_checklist) {
  Sim s;
  s.run(1000, 0.0f);
  CHECK(s.ds.screen() == lt::Screen::Street && !s.ds.checklistVisible());
  s.in.nearCourse = true;  // arrived at the circuit
  uint32_t ev = s.step(0.0f);
  CHECK((ev & lt::kDrvScreen) && (ev & lt::kDrvChecklist));
  CHECK(s.ds.screen() == lt::Screen::Circuit && s.ds.checklistVisible());
  s.run(5000, 40.0f);  // 30..60: stays
  CHECK(s.ds.checklistVisible() && s.ds.sessionOpen());
  CHECK(s.step(60.5f) & lt::kDrvChecklist);
  CHECK(!s.ds.checklistVisible());
  s.run(5000, 50.0f);
  CHECK(!s.ds.checklistVisible());
  s.run(1000, 25.0f);  // back in the pits before a lap started: shown again
  CHECK(s.ds.checklistVisible());
  s.in.lapRunning = true;  // the S/F line was crossed: the run started
  CHECK(s.step(25.0f) & lt::kDrvChecklist);
  CHECK(!s.ds.checklistVisible() && s.ds.hasRun());
  s.run(60000, 120.0f);
  s.run(10000, 20.0f);  // slow in the pit lane after the run: not shown
  CHECK(!s.ds.checklistVisible());
  // parked 30 s: the session closes and the next run gets the checklist again
  // (lapRunning still true in the update that closes the session: the app resets the lap timer then)
  ev = s.run(30040, 0.0f);
  CHECK((ev & lt::kDrvSessionClose) && s.ds.checklistVisible() && !s.ds.hasRun());
  s.in.lapRunning = false;
  s.run(1000, 0.0f);
  CHECK(s.ds.checklistVisible() && s.ds.screen() == lt::Screen::Circuit);
  // leaving the course area: street screen, no checklist
  s.in.nearCourse = false;
  ev = s.step(30.0f);
  CHECK((ev & lt::kDrvScreen) && s.ds.screen() == lt::Screen::Street && !s.ds.checklistVisible());
  // a session with a course stays on the circuit screen outside the 3 km
  s.in.courseSession = true;
  CHECK(s.step(30.0f) & lt::kDrvScreen);
  CHECK(s.ds.screen() == lt::Screen::Circuit && !s.ds.checklistVisible());
}

TEST(drive_odometer_and_track_time) {
  Sim s;
  s.ds.setOdometerKm(12345.6f);
  CHECKF(std::llabs(s.ds.odometerMm() - 12345600000ll) < 1000, "%lld mm", static_cast<long long>(s.ds.odometerMm()));
  const int64_t start = s.ds.odometerMm();
  s.in.nearCourse = true;
  s.step(100.0f);
  s.run(3600000, 100.0f);  // one hour at 100 km/h, 25 Hz
  const int64_t driven = s.ds.odometerMm() - start;
  CHECKF(std::llabs(driven - 100000000ll) < 1000, "driven %lld mm", static_cast<long long>(driven));
  CHECKF(s.ds.trackMs() == 3600000, "track %lld ms", static_cast<long long>(s.ds.trackMs()));
  CHECK(std::fabs(s.ds.trackHours() - 1.0f) < 1e-6f);
  CHECK(std::fabs(s.ds.odometerKm() - 12445.6f) < 0.01f);
  // 20 km/h or slower, or outside the course area: no track time; distance still counts
  int64_t t0 = s.ds.trackMs();
  s.run(10000, 20.0f);
  s.in.nearCourse = false;
  s.run(10000, 80.0f);
  CHECK(s.ds.trackMs() == t0);
  // ramp from 0 to 72 km/h in 10 s at 10 Hz: trapezoid gives exactly 100 m
  lt::DriveState d;
  lt::DriveInput in;
  for (int k = 0; k <= 100; ++k) {
    in.nowMs = 5000 + static_cast<uint32_t>(k) * 100;
    in.canSpeedOk = true;
    in.canSpeedKmh = 0.72f * static_cast<float>(k);
    d.update(in);
  }
  CHECKF(std::llabs(d.odometerMm() - 100000) <= 2, "%lld mm", static_cast<long long>(d.odometerMm()));
  // a gap longer than 1 s adds nothing; GNSS speed is used when CAN has none
  const int64_t before = d.odometerMm();
  in.nowMs += 5000;
  d.update(in);
  CHECK(d.odometerMm() == before);
  in.canSpeedOk = false;
  in.gnssSpeedOk = true;
  in.gnssSpeedKmh = 36.0f;
  for (int k = 0; k < 10; ++k) {
    in.nowMs += 100;
    d.update(in);
  }
  // first step trapezoid 72 -> 36 km/h, then 9 steps at 10 m/s
  CHECKF(std::llabs(d.odometerMm() - before - (1500 + 9000)) <= 2, "%lld mm",
         static_cast<long long>(d.odometerMm() - before));
  // persistence
  lt::DriveState d2;
  d2.setOdometerMm(d.odometerMm());
  d2.setTrackMs(12345);
  CHECK(d2.odometerMm() == d.odometerMm() && d2.trackMs() == 12345);
  d2.reset();
  CHECK(d2.odometerMm() == d.odometerMm() && d2.trackMs() == 12345);  // reset keeps them
}

TEST(drive_checklist_json_and_view) {
  const std::string text = readFile("data/checklist.json");
  CHECKF(!text.empty(), "data/checklist.json not found (run from the repository root)");
  lt::json::MemSource src(text.data(), static_cast<int>(text.size()));
  lt::json::Error e{};
  CHECKF(lt::json::check(src, &e), "json::check: %s at %d:%d", e.why, e.line, e.col);
  lt::ChecklistConfig c;
  c.sport = false;
  CHECKF(lt::loadChecklist(text.data(), static_cast<int>(text.size()), &c, &e), "%s at %d:%d", e.why, e.line, e.col);
  CHECK(c.sport && c.escOff && c.oilOk && !c.racechrono);
  char out[256];
  const int n = lt::checklistJson(c, out, sizeof out);
  lt::ChecklistConfig c2;
  c2.oilOk = false;
  CHECK(n > 0 && lt::loadChecklist(out, n, &c2, &e) && c2.oilOk && !c2.racechrono);
  CHECK(lt::loadChecklist("{\"version\":1,\"racechrono\":true}", -1, &c2, &e) && c2.racechrono && c2.sport);
  CHECK(!lt::loadChecklist("{\"version\":1,\"sport\":1}", -1, &c2, &e) && std::strcmp(e.why, "expected-bool") == 0);
  CHECK(c2.sport && !c2.racechrono);  // a failed load gives the defaults
  CHECK(!lt::loadChecklist("{\"sport\":true}", -1, &c2, &e) && std::strcmp(e.why, "missing-version") == 0);
  CHECK(!lt::loadChecklist("{\"version\":2}", -1, &c2, &e) && std::strcmp(e.why, "bad-version") == 0);
  // the panel: items whose signal is not known are not shown (UI設計 §6)
  lt::ChecklistSignals sig;
  lt::ChecklistView v = lt::evaluateChecklist(c, sig);
  CHECK(v.numShown == 0 && v.open == 0 && !v.ready);
  sig.sportOk = true;
  sig.sport = true;
  sig.escOk = true;
  sig.escOff = false;
  sig.oilOk = true;
  sig.oilWarm = true;
  v = lt::evaluateChecklist(c, sig);
  CHECK(v.numShown == 3 && v.open == 1 && !v.ready && v.shown[lt::kChkEscOff] && !v.done[lt::kChkEscOff]);
  CHECK(!v.shown[lt::kChkRaceChrono]);
  sig.escOff = true;
  v = lt::evaluateChecklist(c, sig);
  CHECK(v.open == 0 && v.ready);
  c.racechrono = true;
  v = lt::evaluateChecklist(c, sig);
  CHECK(v.numShown == 4 && v.open == 1 && !v.ready);
  sig.bleSubscribed = true;
  c.sport = false;  // not used: not shown even though the signal is there
  v = lt::evaluateChecklist(c, sig);
  CHECK(v.numShown == 3 && v.open == 0 && v.ready && !v.shown[lt::kChkSport]);
}

TEST(drive_wifi_json) {
  const std::string text = readFile("data/wifi_example.json");
  CHECKF(!text.empty(), "data/wifi_example.json not found (run from the repository root)");
  lt::json::MemSource src(text.data(), static_cast<int>(text.size()));
  lt::json::Error e{};
  CHECKF(lt::json::check(src, &e), "json::check: %s at %d:%d", e.why, e.line, e.col);
  lt::WifiConfig w{};
  CHECKF(lt::loadWifiConfig(text.data(), static_cast<int>(text.size()), &w, &e), "%s at %d:%d", e.why, e.line, e.col);
  CHECK(std::strcmp(w.hostname, "laptimer") == 0 && w.numNetworks == 2);
  CHECK(std::strcmp(w.networks[0].ssid, "iphone-hotspot-name") == 0 && std::strcmp(w.networks[1].ssid, "home-ssid") == 0);
  CHECK(std::strlen(w.networks[0].pass) >= 8);
  // defaults and limits
  CHECK(lt::loadWifiConfig("{\"version\":1,\"networks\":[{\"ssid\":\"a\"}]}", -1, &w, &e));
  CHECK(std::strcmp(w.hostname, "laptimer") == 0 && w.numNetworks == 1 && w.networks[0].pass[0] == '\0');
  const std::string hex64 = std::string(32, 'a') + std::string(32, '9');
  CHECK(lt::loadWifiConfig(("{\"version\":1,\"networks\":[{\"ssid\":\"" + std::string(32, 's') + "\",\"pass\":\"" +
                            hex64 + "\"}]}")
                               .c_str(),
                           -1, &w, &e));
  struct Bad {
    std::string text;
    const char* why;
  };
  std::string nine = "{\"version\":1,\"networks\":[";
  for (int i = 0; i < 9; ++i) nine += std::string(i ? "," : "") + "{\"ssid\":\"n" + std::to_string(i) + "\"}";
  nine += "]}";
  const Bad bad[] = {
      {"{\"version\":1,\"networks\":[{\"ssid\":\"a\",\"pass\":\"1234567\"}]}", "bad-pass"},
      {"{\"version\":1,\"networks\":[{\"ssid\":\"a\",\"pass\":\"" + std::string(64, 'g') + "\"}]}", "bad-pass"},
      {"{\"version\":1,\"networks\":[{\"ssid\":\"a\",\"pass\":\"" + std::string(65, 'a') + "\"}]}", "bad-pass"},
      {"{\"version\":1,\"networks\":[{\"ssid\":\"" + std::string(33, 's') + "\"}]}", "bad-ssid"},
      {"{\"version\":1,\"networks\":[{\"ssid\":\"\"}]}", "bad-ssid"},
      {"{\"version\":1,\"networks\":[{\"pass\":\"12345678\"}]}", "missing-ssid"},
      {nine, "too-many-networks"},
      {"{\"version\":1,\"hostname\":\"Lap_Timer\",\"networks\":[]}", "bad-hostname"},
      {"{\"version\":1,\"hostname\":\"-lap\",\"networks\":[]}", "bad-hostname"},
      {"{\"version\":1}", "missing-networks"},
      {"{\"networks\":[]}", "missing-version"},
  };
  for (const Bad& b : bad) {
    CHECKF(!lt::loadWifiConfig(b.text.c_str(), -1, &w, &e) && std::strcmp(e.why, b.why) == 0, "expected %s, got %s",
           b.why, e.why);
    CHECK(w.numNetworks == 0 && std::strcmp(w.hostname, "laptimer") == 0);
  }
}
