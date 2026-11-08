// net.h - Wi-Fi station only while the car is stopped (プロジェクト計画 §4.1/§4.7, UI設計 §2).
// Joins the first visible network of the list (wifi.json order = priority, iPhone hotspot first), starts mDNS
// (hostname.local) and tells the app when the network is up or down so it can start/stop the web module.
// Procedure taken from firmware/webcheck (P0-W): scan, first visible in list order, 15 s join timeout.
#pragma once
#include <stdint.h>

namespace net {

constexpr int kMaxNetworks = 8;

struct Network {
  char ssid[33];
  char pass[65];
};

struct Config {
  char hostname[24];  // mDNS name without .local
  int count;
  Network nets[kMaxNetworks];
};

struct Status {
  bool enabled;    // the app allows Wi-Fi now (stopped)
  bool connected;  // joined and has an IP address
  bool mdnsOk;
  char ssid[33];
  char ip[16];
  int rssi;
  uint32_t joins;     // successful joins since boot
  uint32_t attempts;  // join attempts since boot
  char lastError[48];
};

using Hook = void (*)();

// onUp: called (from the network task) after joining + mDNS; onDown: before disconnecting; onLoop: every pass while up
void begin(Hook onUp, Hook onDown, Hook onLoop);
void setConfig(const Config& c);  // may be called any time (e.g. after wifi.json was uploaded)
void setAllowed(bool allowed);    // the drive state decides (stopped = allowed)
void task(void*);                 // FreeRTOS task body
void status(Status* out);
void config(Config* out);  // the networks in use (data page)

}  // namespace net
