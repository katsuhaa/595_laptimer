#include "drive.h"

#include <cmath>
#include <cstring>

namespace lt {

// ---- DriveState ----------------------------------------------------------------------------------

void DriveState::reset() {  // the odometer and the track time are kept
  started_ = false;
  lastMs_ = 0;
  src_ = SpeedSource::None;
  kmh_ = 0.0f;
  prevKnown_ = false;
  prevKmh_ = 0.0f;
  slowOn_ = stillOn_ = moveOn_ = false;
  slowSince_ = stillSince_ = moveSince_ = 0;
  touch_ = true;  // power-on: the car is standing
  stopped_ = false;
  session_ = false;
  wifi_ = false;
  hasRun_ = false;
  slowForList_ = true;
  checklist_ = false;
  screen_ = Screen::Street;
}

// How long `cond` has held (0 when it does not hold now)
uint32_t DriveState::held(bool cond, bool* on, uint32_t* since, uint32_t now) {
  if (!cond) {
    *on = false;
    return 0;
  }
  if (!*on) {
    *on = true;
    *since = now;
  }
  return now - *since;
}

void DriveState::setOdometerKm(float km) {
  if (!(km >= 0.0f)) km = 0.0f;
  const int64_t whole = static_cast<int64_t>(km);
  odoMm_ = whole * 1000000 + std::llround((km - static_cast<float>(whole)) * 1e6f);
  odoFracMm_ = 0.0f;
}

uint32_t DriveState::update(const DriveInput& in) {
  uint32_t ev = 0;
  // speed: CAN first, GNSS when CAN has none
  src_ = SpeedSource::None;
  kmh_ = 0.0f;
  if (in.canSpeedOk && std::isfinite(in.canSpeedKmh)) {
    src_ = SpeedSource::Can;
    kmh_ = in.canSpeedKmh;
  } else if (in.gnssSpeedOk && std::isfinite(in.gnssSpeedKmh)) {
    src_ = SpeedSource::Gnss;
    kmh_ = in.gnssSpeedKmh;
  }
  if (kmh_ < 0.0f) kmh_ = 0.0f;
  const bool known = src_ != SpeedSource::None;
  const float v = kmh_;

  // distance and track time over the step since the last call
  const uint32_t dt = started_ ? in.nowMs - lastMs_ : 0;
  started_ = true;
  lastMs_ = in.nowMs;
  const bool stepOk = dt > 0 && dt <= kMaxStepMs;
  if (stepOk && known && prevKnown_) {
    odoFracMm_ += (v + prevKmh_) * 0.5f / 3.6f * static_cast<float>(dt);  // m/s * ms = mm
    const int64_t whole = static_cast<int64_t>(odoFracMm_);
    odoMm_ += whole;
    odoFracMm_ -= static_cast<float>(whole);
  }
  if (stepOk && known && in.nearCourse && v > kTrackTimeKmh) trackMs_ += dt;
  prevKnown_ = known;
  prevKmh_ = v;

  // stopped: speed 0 (or no speed at all) for kStopMs
  const bool still = !known || v < kStopKmh;
  const uint32_t stillMs = held(still, &stillOn_, &stillSince_, in.nowMs);
  const bool wasStopped = stopped_;
  if (!still) {
    stopped_ = false;
  } else if (stillMs >= kStopMs) {
    stopped_ = true;
  }
  if (stopped_ && !wasStopped) ev |= kDrvStopped;

  // touch: < 10 km/h for 2 s on, > 15 km/h off; Wi-Fi: on when stopped, off > 15 km/h
  const bool fast = known && v > kTouchOffKmh;
  const uint32_t slowMs = held(known && v < kTouchOnKmh, &slowOn_, &slowSince_, in.nowMs);
  const bool wasTouch = touch_;
  if (fast) {
    touch_ = false;
  } else if (slowMs >= kTouchOnMs || stopped_) {
    touch_ = true;
  }
  if (touch_ != wasTouch) ev |= touch_ ? kDrvTouchOn : kDrvTouchOff;
  const bool wasWifi = wifi_;
  if (fast) {
    wifi_ = false;
  } else if (stopped_) {
    wifi_ = true;
  }
  if (wifi_ != wasWifi) ev |= wifi_ ? kDrvWifiOn : kDrvWifiOff;

  // session: opens when moving (> 10 km/h for 3 s), closes when stopped
  const uint32_t moveMs = held(known && v > kMoveKmh, &moveOn_, &moveSince_, in.nowMs);
  const bool wasSession = session_;
  if (stopped_) {
    session_ = false;
  } else if (moveMs >= kMoveMs) {
    session_ = true;
  }
  if (session_ != wasSession) ev |= session_ ? kDrvSessionOpen : kDrvSessionClose;

  // run = a lap has started in the open session; forgotten outside the course area and at the close
  if (!in.nearCourse || (stopped_ && !wasStopped)) hasRun_ = false;
  if (session_ && in.lapRunning) hasRun_ = true;
  if (known && v < kChecklistShowKmh) {
    slowForList_ = true;
  } else if (known && v > kChecklistHideKmh) {
    slowForList_ = false;
  }
  const bool list = in.nearCourse && !hasRun_ && slowForList_;
  if (list != checklist_) ev |= kDrvChecklist;
  checklist_ = list;

  const Screen scr = in.nearCourse || in.courseSession ? Screen::Circuit : Screen::Street;
  if (scr != screen_) ev |= kDrvScreen;
  screen_ = scr;
  return ev;
}

// ---- checklist.json ------------------------------------------------------------------------------

namespace {

bool readChecklist(json::Reader& r, ChecklistConfig* c) {
  bool haveVersion = false;
  if (!r.beginObject()) return false;
  while (r.nextKey()) {
    if (r.keyIs("version")) {
      int32_t v = 0;
      if (!r.readInt(&v)) return false;
      if (v != 1) return r.fail("bad-version");
      haveVersion = true;
    } else if (r.keyIs("sport")) {
      if (!r.readBool(&c->sport)) return false;
    } else if (r.keyIs("esc_off")) {
      if (!r.readBool(&c->escOff)) return false;
    } else if (r.keyIs("oil_ok")) {
      if (!r.readBool(&c->oilOk)) return false;
    } else if (r.keyIs("racechrono")) {
      if (!r.readBool(&c->racechrono)) return false;
    } else if (!r.skip()) {
      return false;
    }
  }
  if (!r.finish()) return false;
  if (!haveVersion) return r.fail("missing-version");
  return true;
}

bool validHostname(const char* h, int n) {
  if (n < 1 || n > 31 || h[0] == '-' || h[n - 1] == '-') return false;
  for (int i = 0; i < n; ++i) {
    const char c = h[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  }
  return true;
}

bool validPass(const char* p, int n) {
  if (n == 0) return true;  // open network
  if (n == 64) {
    for (int i = 0; i < n; ++i) {
      const char c = p[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
  }
  if (n < 8 || n > 63) return false;
  for (int i = 0; i < n; ++i) {
    if (p[i] < 0x20 || p[i] > 0x7E) return false;  // WPA passphrase: printable ASCII
  }
  return true;
}

bool readWifi(json::Reader& r, WifiConfig* c) {
  bool haveVersion = false;
  bool haveNetworks = false;
  if (!r.beginObject()) return false;
  while (r.nextKey()) {
    if (r.keyIs("version")) {
      int32_t v = 0;
      if (!r.readInt(&v)) return false;
      if (v != 1) return r.fail("bad-version");
      haveVersion = true;
    } else if (r.keyIs("hostname")) {
      const int n = r.readString(c->hostname, sizeof c->hostname);
      if (n < 0) return false;
      if (n >= static_cast<int>(sizeof c->hostname) || !validHostname(c->hostname, n)) {
        return r.fail("bad-hostname");
      }
    } else if (r.keyIs("networks")) {
      if (!r.beginArray()) return false;
      while (r.nextItem()) {
        if (c->numNetworks >= kWifiMaxNetworks) return r.fail("too-many-networks");
        WifiNetwork& w = c->networks[c->numNetworks];
        w = WifiNetwork{};
        bool haveSsid = false;
        if (!r.beginObject()) return false;
        while (r.nextKey()) {
          if (r.keyIs("ssid")) {
            const int n = r.readString(w.ssid, sizeof w.ssid);
            if (n < 0) return false;
            if (n < 1 || n >= static_cast<int>(sizeof w.ssid)) return r.fail("bad-ssid");
            haveSsid = true;
          } else if (r.keyIs("pass")) {
            const int n = r.readString(w.pass, sizeof w.pass);
            if (n < 0) return false;
            if (n >= static_cast<int>(sizeof w.pass) || !validPass(w.pass, n)) return r.fail("bad-pass");
          } else if (!r.skip()) {
            return false;
          }
        }
        if (!r.ok()) return false;
        if (!haveSsid) return r.fail("missing-ssid");
        ++c->numNetworks;
      }
      if (!r.ok()) return false;
      haveNetworks = true;
    } else if (!r.skip()) {
      return false;
    }
  }
  if (!r.finish()) return false;
  if (!haveVersion) return r.fail("missing-version");
  if (!haveNetworks) return r.fail("missing-networks");
  return true;
}

}  // namespace

bool loadChecklist(json::Source& src, ChecklistConfig* out, json::Error* err) {
  json::Reader r(src);
  ChecklistConfig c;
  const bool ok = readChecklist(r, &c);
  *out = ok ? c : ChecklistConfig();
  if (err) *err = ok ? json::Error{0, 0, ""} : r.error();
  return ok;
}

bool loadChecklist(const char* text, int len, ChecklistConfig* out, json::Error* err) {
  json::MemSource src(text, len);
  return loadChecklist(src, out, err);
}

int checklistJson(const ChecklistConfig& c, char* out, int cap) {
  json::Writer w(out, cap);
  w.beginObject().key("version").i64(1).key("sport").boolean(c.sport).key("esc_off").boolean(c.escOff);
  w.key("oil_ok").boolean(c.oilOk).key("racechrono").boolean(c.racechrono).endObject();
  return w.finish();
}

ChecklistView evaluateChecklist(const ChecklistConfig& cfg, const ChecklistSignals& sig) {
  ChecklistView v{};
  v.shown[kChkSport] = cfg.sport && sig.sportOk;
  v.done[kChkSport] = sig.sport;
  v.shown[kChkEscOff] = cfg.escOff && sig.escOk;
  v.done[kChkEscOff] = sig.escOff;
  v.shown[kChkOilOk] = cfg.oilOk && sig.oilOk;
  v.done[kChkOilOk] = sig.oilWarm;
  v.shown[kChkRaceChrono] = cfg.racechrono;
  v.done[kChkRaceChrono] = sig.bleSubscribed;
  for (int i = 0; i < kChkCount; ++i) {
    if (!v.shown[i]) {
      v.done[i] = false;
      continue;
    }
    ++v.numShown;
    if (!v.done[i]) ++v.open;
  }
  v.ready = v.numShown > 0 && v.open == 0;
  return v;
}

// ---- wifi.json -----------------------------------------------------------------------------------

bool loadWifiConfig(json::Source& src, WifiConfig* out, json::Error* err) {
  json::Reader r(src);
  *out = WifiConfig{};
  std::memcpy(out->hostname, "laptimer", 9);
  const bool ok = readWifi(r, out);
  if (!ok) {
    *out = WifiConfig{};
    std::memcpy(out->hostname, "laptimer", 9);
  }
  if (err) *err = ok ? json::Error{0, 0, ""} : r.error();
  return ok;
}

bool loadWifiConfig(const char* text, int len, WifiConfig* out, json::Error* err) {
  json::MemSource src(text, len);
  return loadWifiConfig(src, out, err);
}

}  // namespace lt
