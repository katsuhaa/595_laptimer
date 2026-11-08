// net.h - Wi-Fi station only while the car is stopped (プロジェクト計画 §4.1/§4.7, UI設計 §2).
// The networks are registered on the device screen (settings > Wi-Fi, the password typed on the on-screen keyboard)
// and kept in NVS, so they work without the XTSD; an uploaded wifi.json adds to them (importNetworks).
// Joins the registered networks that a scan sees, in list order (the one saved last first), starts mDNS
// (hostname.local) and tells the app when the network is up or down so it can start/stop the web module.
// Procedure from firmware/webcheck (P0-W): scan, list order, 15 s join timeout.
#pragma once
#include <stdint.h>

namespace net {

constexpr int kMaxNetworks = 8;
constexpr int kMaxFound = 16;

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
  bool enabled;    // Wi-Fi may be on now (stopped, or the Wi-Fi page is open)
  bool connected;  // joined and has an IP address
  bool mdnsOk;
  char ssid[33];
  char ip[16];
  int rssi;
  uint32_t joins;     // successful joins since boot
  uint32_t attempts;  // join attempts since boot
  char lastError[48];
};

// Networks seen by the last scan (for the Wi-Fi page)
struct Found {
  char ssid[33];
  int8_t rssi;
  bool locked;  // needs a password
};
struct ScanView {
  bool scanning;  // requested and not finished
  bool ok;        // found[] holds a finished scan
  int n;
  Found found[kMaxFound];  // strongest first, hidden networks left out
};

// The registered network names and the mDNS name (the passwords never leave this module)
struct Names {
  char hostname[24];
  int count;
  char ssid[kMaxNetworks][33];  // in the order they are tried
};

// Result of joining the network registered last (saveNetwork)
enum Join : uint8_t { J_IDLE = 0, J_TRYING, J_OK, J_NOT_FOUND, J_BAD_PASSWORD, J_FAILED };

using Hook = void (*)();

// Loads the registered networks from NVS. onUp: called (from the network task) after joining + mDNS;
// onDown: before disconnecting; onLoop: every pass while up
void begin(Hook onUp, Hook onDown, Hook onLoop);
void setAllowed(bool allowed);  // the drive state decides (stopped, no session = allowed)
void setSetup(bool on);         // the Wi-Fi page is open: the radio may scan and join too
void task(void*);               // FreeRTOS task body
void status(Status* out);
void names(Names* out);

// Registered networks (written to NVS). saveNetwork adds ssid or replaces its password, puts it first and joins it
// now (joinView follows the result). false: bad ssid / password, or NVS write failed.
bool saveNetwork(const char* ssid, const char* pass);
bool forgetNetwork(const char* ssid);
// wifi.json upload: unknown networks are added at the end (up to kMaxNetworks), known ones get the file's password;
// the hostname is taken when given. Returns the number of networks added or changed, -1 if NVS failed.
int importNetworks(const Config& c);

void requestScan();
void scanView(ScanView* out);
void joinView(uint8_t* state, char ssid[33]);  // Join, and the network it is about

// Wi-Fi rules (WPA2): open = "", else 8..63 characters or exactly 64 hex digits; ssid 1..32 bytes
bool validSsid(const char* ssid);
bool validPass(const char* pass);

}  // namespace net
