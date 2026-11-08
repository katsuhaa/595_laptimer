// net.cpp - see net.h
#include "net.h"

#include <ESPmDNS.h>
#include <WiFi.h>
#include <string.h>

#include "../hw/nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace net {
namespace {

constexpr uint32_t kJoinWaitMs = 15000;
constexpr uint32_t kRetryMs = 10000;
constexpr const char* kNvsKey = "wifi";  // the registered networks (Config)
constexpr uint16_t kNvsVersion = 1;

SemaphoreHandle_t g_mu = nullptr;
Config g_cfg = {};
Status g_st = {};
volatile bool g_allowed = false;
volatile bool g_setup = false;
volatile bool g_cfgChanged = false;  // the list changed: join again from the start of the list
volatile bool g_scanReq = false;
ScanView g_scan = {};
uint8_t g_join = J_IDLE;  // under g_mu
char g_joinSsid[33] = "";
volatile uint8_t g_reason = 0;  // reason of the last station disconnect (set from the Wi-Fi event task)
Hook g_onUp = nullptr;
Hook g_onDown = nullptr;
Hook g_onLoop = nullptr;

void lock() { xSemaphoreTake(g_mu, portMAX_DELAY); }
void unlock() { xSemaphoreGive(g_mu); }

bool radioWanted() { return g_allowed || g_setup; }

void setError(const char* e) {
  lock();
  strncpy(g_st.lastError, e, sizeof g_st.lastError - 1);
  g_st.lastError[sizeof g_st.lastError - 1] = 0;
  unlock();
}

// The result for `ssid`, unless another network was saved meanwhile (its result is pending)
void setJoinFor(const char* ssid, uint8_t j) {
  lock();
  if (strcmp(g_joinSsid, ssid) == 0) g_join = j;
  unlock();
}

void copyStr(char* dst, size_t cap, const char* src) {
  strncpy(dst, src ? src : "", cap - 1);
  dst[cap - 1] = 0;
}

int find(const Config& c, const char* ssid) {
  for (int k = 0; k < c.count; ++k) {
    if (strcmp(c.nets[k].ssid, ssid) == 0) return k;
  }
  return -1;
}

bool store(const Config& c) { return hw::nvsSave(kNvsKey, &c, sizeof c, kNvsVersion); }

bool radioOn = false;
bool up = false;

void radioStart(const char* host) {
  WiFi.setHostname(host);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // full speed while transferring (P0-W)
  WiFi.setAutoReconnect(false);  // reconnecting is done here, in list order
  radioOn = true;
}

void goDown() {
  if (up) {
    if (g_onDown) g_onDown();
    MDNS.end();
    up = false;
  }
  if (radioOn) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    radioOn = false;
  }
  lock();
  g_st.connected = false;
  g_st.mdnsOk = false;
  g_st.ip[0] = 0;
  g_st.ssid[0] = 0;
  g_st.rssi = 0;
  unlock();
}

// Scans (blocking, 2-3 s), keeps the strongest kMaxFound names for the Wi-Fi page and marks which registered
// networks are visible (c may be null). Returns the number of access points seen, < 0 on failure.
int scan(const Config* c, bool seen[kMaxNetworks]) {
  lock();
  g_scan.scanning = true;
  unlock();
  const int n = WiFi.scanNetworks();
  Found tmp[kMaxFound];
  int k = 0;
  for (int i = 0; i < n; ++i) {
    const String s = WiFi.SSID(i);
    if (!s.length() || s.length() > 32) continue;  // hidden network
    for (int j = 0; c && j < c->count; ++j) {
      if (s == c->nets[j].ssid) seen[j] = true;
    }
    const int8_t rssi = static_cast<int8_t>(WiFi.RSSI(i));
    const bool locked = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    int at = -1;  // same name from several access points: the strongest one
    for (int j = 0; j < k; ++j) {
      if (s == tmp[j].ssid) at = j;
    }
    if (at >= 0) {
      if (rssi > tmp[at].rssi) tmp[at].rssi = rssi;
      continue;
    }
    if (k < kMaxFound) {
      at = k++;
    } else {  // full: replace the weakest if this one is stronger
      at = 0;
      for (int j = 1; j < k; ++j) {
        if (tmp[j].rssi < tmp[at].rssi) at = j;
      }
      if (tmp[at].rssi >= rssi) continue;
    }
    copyStr(tmp[at].ssid, sizeof tmp[at].ssid, s.c_str());
    tmp[at].rssi = rssi;
    tmp[at].locked = locked;
  }
  WiFi.scanDelete();
  for (int i = 1; i < k; ++i) {  // strongest first
    for (int j = i; j > 0 && tmp[j].rssi > tmp[j - 1].rssi; --j) {
      const Found t = tmp[j];
      tmp[j] = tmp[j - 1];
      tmp[j - 1] = t;
    }
  }
  lock();
  memcpy(g_scan.found, tmp, sizeof(Found) * static_cast<size_t>(k));
  g_scan.n = k;
  g_scan.ok = n >= 0;
  g_scan.scanning = false;
  unlock();
  return n;
}

uint8_t classify(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return J_BAD_PASSWORD;
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
      return J_NOT_FOUND;
    default:
      return J_FAILED;
  }
}

// The registered networks that the scan sees, in list order (webcheck connectInOrder); the next one when a join
// fails. `target` = the network saved last (its result goes to joinView).
bool joinInOrder(const Config& c, const char* target) {
  bool seen[kMaxNetworks] = {};
  const int n = scan(&c, seen);
  const int t = target[0] ? find(c, target) : -1;
  if (target[0] && (t < 0 || !seen[t])) setJoinFor(target, J_NOT_FOUND);
  for (int k = 0; k < c.count; ++k) {
    if (!seen[k] || !radioWanted() || g_cfgChanged) continue;
    lock();
    g_st.attempts++;
    unlock();
    g_reason = 0;
    WiFi.begin(c.nets[k].ssid, c.nets[k].pass[0] ? c.nets[k].pass : nullptr);
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < kJoinWaitMs && radioWanted() && !g_cfgChanged) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (WiFi.status() == WL_CONNECTED) {
      if (k == t) setJoinFor(target, J_OK);
      return true;
    }
    if (k == t && !g_cfgChanged) setJoinFor(target, g_reason ? classify(g_reason) : J_FAILED);
    WiFi.disconnect();
  }
  setError(n <= 0 ? "no networks visible" : c.count ? "no registered network joined" : "no network registered");
  return false;
}

}  // namespace

void begin(Hook onUp, Hook onDown, Hook onLoop) {
  g_mu = xSemaphoreCreateMutex();
  g_onUp = onUp;
  g_onDown = onDown;
  g_onLoop = onLoop;
  Config c = {};
  if (!hw::nvsLoad(kNvsKey, &c, sizeof c, kNvsVersion) || c.count < 0 || c.count > kMaxNetworks) c = Config{};
  c.hostname[sizeof c.hostname - 1] = 0;
  if (!c.hostname[0]) copyStr(c.hostname, sizeof c.hostname, "laptimer");
  g_cfg = c;
  WiFi.onEvent(
      [](arduino_event_id_t, arduino_event_info_t info) { g_reason = info.wifi_sta_disconnected.reason; },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
}

void setAllowed(bool allowed) { g_allowed = allowed; }
void setSetup(bool on) { g_setup = on; }

void status(Status* out) {
  lock();
  *out = g_st;
  out->enabled = radioWanted();
  unlock();
}

void names(Names* out) {
  lock();
  memcpy(out->hostname, g_cfg.hostname, sizeof out->hostname);
  out->count = g_cfg.count;
  for (int k = 0; k < kMaxNetworks; ++k) memcpy(out->ssid[k], g_cfg.nets[k].ssid, sizeof out->ssid[k]);
  unlock();
}

bool validSsid(const char* ssid) {
  const size_t n = ssid ? strlen(ssid) : 0;
  return n >= 1 && n <= 32;
}

bool validPass(const char* pass) {
  const size_t n = pass ? strlen(pass) : 0;
  if (n == 0) return true;
  for (size_t i = 0; i < n; ++i) {
    if (pass[i] < 0x20 || pass[i] > 0x7E) return false;
  }
  if (n >= 8 && n <= 63) return true;
  if (n != 64) return false;
  for (size_t i = 0; i < n; ++i) {
    const char ch = pass[i];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) return false;
  }
  return true;
}

bool saveNetwork(const char* ssid, const char* pass) {
  if (!validSsid(ssid) || !validPass(pass)) return false;
  lock();
  Config c = g_cfg;
  const int at = find(c, ssid);
  // remove it (or the last one when the list is full), then put it first
  const int drop = at >= 0 ? at : (c.count == kMaxNetworks ? kMaxNetworks - 1 : -1);
  if (drop >= 0) {
    for (int k = drop; k + 1 < c.count; ++k) c.nets[k] = c.nets[k + 1];
    c.count--;
  }
  for (int k = c.count; k > 0; --k) c.nets[k] = c.nets[k - 1];
  c.nets[0] = Network{};
  copyStr(c.nets[0].ssid, sizeof c.nets[0].ssid, ssid);
  copyStr(c.nets[0].pass, sizeof c.nets[0].pass, pass);
  c.count++;
  const bool ok = store(c);
  if (ok) g_cfg = c;
  g_join = ok ? J_TRYING : J_FAILED;  // the page shows 接続できません when it could not be stored
  copyStr(g_joinSsid, sizeof g_joinSsid, ssid);
  unlock();
  memset(&c, 0, sizeof c);  // no copy of the passwords left on the stack
  if (ok) g_cfgChanged = true;
  return ok;
}

bool forgetNetwork(const char* ssid) {
  lock();
  Config c = g_cfg;
  const int at = ssid ? find(c, ssid) : -1;
  bool ok = at >= 0;
  if (ok) {
    for (int k = at; k + 1 < c.count; ++k) c.nets[k] = c.nets[k + 1];
    c.count--;
    c.nets[c.count] = Network{};
    ok = store(c);
    if (ok) g_cfg = c;
  }
  const bool joined = ok && strcmp(g_st.ssid, ssid) == 0;
  if (ok && strcmp(g_joinSsid, ssid) == 0) {
    g_join = J_IDLE;
    g_joinSsid[0] = 0;
  }
  unlock();
  memset(&c, 0, sizeof c);
  if (joined) g_cfgChanged = true;  // it was the network in use: join another one
  return ok;
}

int importNetworks(const Config& in) {
  lock();
  Config c = g_cfg;
  int changed = 0;
  if (in.hostname[0] && strcmp(in.hostname, c.hostname) != 0) {
    copyStr(c.hostname, sizeof c.hostname, in.hostname);
    ++changed;
  }
  for (int i = 0; i < in.count && i < kMaxNetworks; ++i) {
    const Network& n = in.nets[i];
    if (!validSsid(n.ssid) || !validPass(n.pass)) continue;
    const int at = find(c, n.ssid);
    if (at >= 0) {
      if (strcmp(c.nets[at].pass, n.pass) != 0) {
        copyStr(c.nets[at].pass, sizeof c.nets[at].pass, n.pass);
        ++changed;
      }
    } else if (c.count < kMaxNetworks) {
      c.nets[c.count] = Network{};
      copyStr(c.nets[c.count].ssid, sizeof c.nets[c.count].ssid, n.ssid);
      copyStr(c.nets[c.count].pass, sizeof c.nets[c.count].pass, n.pass);
      c.count++;
      ++changed;
    }
  }
  bool ok = true;
  if (changed) {
    ok = store(c);
    if (ok) g_cfg = c;
  }
  unlock();
  memset(&c, 0, sizeof c);
  if (changed && ok) g_cfgChanged = true;
  return ok ? changed : -1;
}

void requestScan() {
  lock();
  g_scan.scanning = true;  // shown at once; the network task scans as soon as the radio is free
  unlock();
  g_scanReq = true;
}

void scanView(ScanView* out) {
  lock();
  *out = g_scan;
  unlock();
}

void joinView(uint8_t* state, char ssid[33]) {
  lock();
  *state = g_join;
  memcpy(ssid, g_joinSsid, sizeof g_joinSsid);
  unlock();
}

void task(void*) {
  uint32_t lastTry = 0;
  bool first = true;
  for (;;) {
    if (!radioWanted() || g_cfgChanged) {
      if (g_cfgChanged) first = true;  // the list changed: join again now
      g_cfgChanged = false;
      goDown();
      if (!radioWanted()) {
        if (g_scanReq) {  // no radio now (moving): nothing to show
          g_scanReq = false;
          lock();
          g_scan.scanning = false;
          unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }
    }
    char host[sizeof g_cfg.hostname];
    lock();
    memcpy(host, g_cfg.hostname, sizeof host);
    unlock();
    if (!radioOn) radioStart(host);
    if (g_scanReq) {  // the Wi-Fi page asked (also while joined)
      g_scanReq = false;
      bool seen[kMaxNetworks] = {};
      scan(nullptr, seen);
    }
    if (!up) {
      if (!first && millis() - lastTry < kRetryMs) {
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }
      first = false;
      lastTry = millis();
      Config c;
      char target[33];
      uint8_t j;
      lock();
      c = g_cfg;
      j = g_join;
      memcpy(target, g_joinSsid, sizeof target);
      unlock();
      if (j != J_TRYING) target[0] = 0;
      if (c.count == 0) {
        setError("no network registered");
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
      }
      const bool joined = joinInOrder(c, target);
      memset(&c, 0, sizeof c);  // no copy of the passwords left on the stack
      if (!joined) continue;
      const bool mdns = MDNS.begin(host);
      if (mdns) MDNS.addService("http", "tcp", 80);
      lock();
      g_st.connected = true;
      g_st.mdnsOk = mdns;
      g_st.joins++;
      strncpy(g_st.ssid, WiFi.SSID().c_str(), sizeof g_st.ssid - 1);
      strncpy(g_st.ip, WiFi.localIP().toString().c_str(), sizeof g_st.ip - 1);
      g_st.rssi = WiFi.RSSI();
      g_st.lastError[0] = 0;
      unlock();
      up = true;
      if (g_onUp) g_onUp();
      continue;
    }
    if (WiFi.status() != WL_CONNECTED) {
      setError("connection lost");
      goDown();
      continue;
    }
    if (g_onLoop) g_onLoop();
    static uint32_t rssiMs = 0;
    if (millis() - rssiMs > 2000) {
      rssiMs = millis();
      lock();
      g_st.rssi = WiFi.RSSI();
      unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

}  // namespace net
