// state.h - what the tasks share (a snapshot is copied under one lock; nobody keeps a pointer into it)
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "lt_core.h"

namespace app {

constexpr const char* kFwVersion = "0.1.0";

// Scratch memory in PSRAM for one call: keeps large buffers out of the internal RAM (Wi-Fi, BLE and the task
// stacks need it). ok() is false when PSRAM is exhausted.
class PsBuf {
 public:
  explicit PsBuf(size_t bytes) : p_(heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) {}
  ~PsBuf() { heap_caps_free(p_); }
  PsBuf(const PsBuf&) = delete;
  PsBuf& operator=(const PsBuf&) = delete;
  bool ok() const { return p_ != nullptr; }
  template <class T>
  T* as() const {
    return static_cast<T*>(p_);
  }

 private:
  void* p_;
};

enum class Phase : uint8_t { SelfTest, Run };

struct SelfTestRow {
  enum State : uint8_t { Wait, Pass, Fail, Info } state;
  char label[24];
  char value[48];
};
constexpr int kSelfTestRows = 8;

struct Live {
  Phase phase;
  // GNSS
  bool gnssUp;               // NAV-PVT arriving
  lt::GnssFix fix;           // newest
  uint32_t fixMs;            // millis() of the newest fix
  uint32_t pvtPerSec;
  uint32_t missedEpochs;
  bool gnssConfigured;
  uint32_t gnssBaud;
  // time
  bool timeOk;
  int64_t utcMsAtFix;        // UTC of the newest fix (or RTC time at boot)
  uint32_t utcBaseMs;        // millis() that belongs to utcMsAtFix
  bool rtcOk;
  // vehicle (CAN link)
  lt::VehValues veh;
  bool linkUp;
  uint32_t canPerSec;
  bool lightOn;
  // IMU (car frame, averaged over the last GNSS epoch)
  bool imuOk;
  float lonG, latG, vertG, yawDps;
  bool imuLevel, imuForward;
  // drive state
  bool touchEnabled, stopped, wifiAllowed, checklistVisible;
  lt::Screen screen;
  bool speedOk;
  float speedKmh;
  float odoKm, trackHours;
  int64_t odoMm, trackMs;  // the same, exact (kept in NVS)
  // peak hold (core/peakhold, round gauge values)
  bool peakVisible;
  float peakMarkerRpm, peakTopRpm;
  // session
  bool sessionOpen;
  char sessionId[16];
  float maxKmh;
  int maxRpm;
  // checklist
  lt::ChecklistView checklist;
  // BLE
  bool bleOn, bleConnected;
  // storage
  bool sdOk;
  uint64_t sdTotal, sdUsed;
  float remainHours;
  bool storageWarn;
  int undownloaded;
  // self-test
  SelfTestRow selftest[kSelfTestRows];
  bool selftestAllPass;
};

void liveGet(Live* out);

// For ui_glue.cpp (implemented in app.cpp)
void setPhaseRun();
void setRecording(bool on);
bool recording();
void settingsChanged();
void requestConfigReload(uint32_t mask);  // 1 courses, 2 maint, 4 checklist, 8 wifi
bool writeConfigFile(const char* name, const char* text, int len);  // /config/<name> (atomic), then reload
lt::Maint* maintRef();
lt::CourseDb* coursesRef();
const lt::ChecklistConfig& checklistRef();
void* lockRef();  // FreeRTOS mutex of the config objects (SemaphoreHandle_t)

}  // namespace app
