// net.cpp - see net.h
#include "net.h"

#include <ESPmDNS.h>
#include <WiFi.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace net {
namespace {

constexpr uint32_t kJoinWaitMs = 15000;
constexpr uint32_t kRetryMs = 10000;

SemaphoreHandle_t g_mu = nullptr;
Config g_cfg = {};
Status g_st = {};
volatile bool g_allowed = false;
volatile bool g_cfgChanged = false;
Hook g_onUp = nullptr;
Hook g_onDown = nullptr;
Hook g_onLoop = nullptr;

void lock() { xSemaphoreTake(g_mu, portMAX_DELAY); }
void unlock() { xSemaphoreGive(g_mu); }

void setError(const char* e) {
  lock();
  strncpy(g_st.lastError, e, sizeof g_st.lastError - 1);
  g_st.lastError[sizeof g_st.lastError - 1] = 0;
  unlock();
}

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

// The first network of the list that the scan sees (webcheck connectInOrder)
bool joinInOrder(const Config& c) {
  const int n = WiFi.scanNetworks();
  bool seen[kMaxNetworks] = {};
  for (int i = 0; i < n; ++i) {
    for (int k = 0; k < c.count; ++k) {
      if (WiFi.SSID(i) == c.nets[k].ssid) seen[k] = true;
    }
  }
  WiFi.scanDelete();
  for (int k = 0; k < c.count; ++k) {
    if (!seen[k] || !g_allowed) continue;
    lock();
    g_st.attempts++;
    unlock();
    WiFi.begin(c.nets[k].ssid, c.nets[k].pass);
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < kJoinWaitMs && g_allowed) vTaskDelay(pdMS_TO_TICKS(100));
    if (WiFi.status() == WL_CONNECTED) return true;
    WiFi.disconnect();
  }
  setError(n <= 0 ? "no networks visible" : "none of wifi.json joined");
  return false;
}

}  // namespace

void begin(Hook onUp, Hook onDown, Hook onLoop) {
  g_mu = xSemaphoreCreateMutex();
  g_onUp = onUp;
  g_onDown = onDown;
  g_onLoop = onLoop;
  strcpy(g_cfg.hostname, "laptimer");
}

void setConfig(const Config& c) {
  lock();
  g_cfg = c;
  if (!g_cfg.hostname[0]) strcpy(g_cfg.hostname, "laptimer");
  unlock();
  g_cfgChanged = true;
}

void setAllowed(bool allowed) { g_allowed = allowed; }

void status(Status* out) {
  lock();
  *out = g_st;
  out->enabled = g_allowed;
  unlock();
}

void config(Config* out) {
  lock();
  *out = g_cfg;
  unlock();
}

void task(void*) {
  uint32_t lastTry = 0;
  bool first = true;
  for (;;) {
    if (!g_allowed || g_cfgChanged) {
      g_cfgChanged = false;
      goDown();
      if (!g_allowed) {
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }
    }
    if (!up) {
      if (!first && millis() - lastTry < kRetryMs) {
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }
      first = false;
      lastTry = millis();
      Config c;
      lock();
      c = g_cfg;
      unlock();
      if (c.count == 0) {
        setError("wifi.json has no networks");
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      if (!radioOn) radioStart(c.hostname);
      if (!joinInOrder(c)) continue;
      const bool mdns = MDNS.begin(c.hostname);
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
