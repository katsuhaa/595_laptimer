// ble_diy.cpp - see ble_diy.h. Port of the round gauge's RaceChrono part of meter_display.ino (05ab0a1):
// bleBegin, RcServerCb, rcGapEvent, RcFilterCb, RcMonitorCb, RcNotifyCb, bleTask, rcPush, platCanFrame and the
// IMU/GPS sends of imuPoll/gpsOnPvt. Serial lines say the same as the round gauge's (in English here).
#include "ble_diy.h"

#include <Arduino.h>
#include <string.h>

// arduino-esp32 3.x: a sketch that uses an external BLE library must include this header, otherwise the core frees
// the BLE controller memory at start-up (it sets _bleLibraryInUse; see esp32-hal-bt.c)
#include "esp32-hal-alloc-ble-mem.h"
#if __has_include(<NimBLELocalValueAttribute.h>)
#error "NimBLE-Arduino 1.4.3 is required (select version 1.4.3 in the Library Manager). 2.x has a different API."
#endif
#include <NimBLEDevice.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace hw {

namespace {

enum : uint8_t { RC_CAN = 0, RC_GPS = 1, RC_TIME = 2 };
struct RcMsg {
  uint8_t kind, len;
  uint8_t data[20];
};

QueueHandle_t s_qGps = nullptr, s_qCan = nullptr;
TaskHandle_t s_task = nullptr;
SemaphoreHandle_t s_mutex = nullptr;  // filter and GPS sync (BLE write callback + senders)
lt::rcdiy::Filter s_filter;
lt::rcdiy::GpsTimeSync s_sync;
NimBLECharacteristic *s_chCan = nullptr, *s_chFilter = nullptr, *s_chGps = nullptr, *s_chTime = nullptr,
                     *s_chMonCfg = nullptr, *s_chMonVal = nullptr;
volatile bool s_ok = false, s_connected = false;
volatile uint8_t s_subscribed = 0;
volatile uint16_t s_mtu = 0;
// counts only (read from other tasks; a lost increment does not matter, as in the round gauge)
uint32_t s_canSent = 0, s_gpsSent = 0, s_imuSent = 0, s_dropped = 0, s_connects = 0;
uint32_t s_connMs = 0;
uint32_t s_imuSendMs = 0;
uint32_t s_imuIdClash = 0, s_imuIdClashMs = 0;

void lock() { xSemaphoreTake(s_mutex, portMAX_DELAY); }
void unlock() { xSemaphoreGive(s_mutex); }

bool filterPass(uint32_t pid, uint32_t now, uint16_t minMs) {
  lock();
  const bool ok = s_filter.pass(pid, now, minMs);
  unlock();
  return ok;
}

void push(uint8_t kind, const uint8_t* data, size_t len) {
  if (len > sizeof(RcMsg::data)) return;
  RcMsg m;
  m.kind = kind;
  m.len = static_cast<uint8_t>(len);
  memcpy(m.data, data, len);
  QueueHandle_t q = kind == RC_CAN ? s_qCan : s_qGps;
  if (xQueueSend(q, &m, 0) != pdTRUE) {
    s_dropped++;
    if (kind == RC_CAN) return;
    RcMsg old;  // GPS: the newest epoch is worth more than the oldest waiting one
    xQueueReceive(q, &old, 0);
    if (xQueueSend(q, &m, 0) != pdTRUE) return;
  }
  xTaskNotifyGive(s_task);
}

uint8_t bitOf(const NimBLECharacteristic* ch) {
  return ch == s_chCan ? 1 : ch == s_chGps ? 2 : ch == s_chTime ? 4 : 0;
}

const char* chName(const NimBLECharacteristic* ch) {
  return ch == s_chCan      ? "CAN"
         : ch == s_chGps    ? "GPS"
         : ch == s_chTime   ? "GPS time"
         : ch == s_chFilter ? "filter"
         : ch == s_chMonCfg ? "monitor config"
         : ch == s_chMonVal ? "monitor value"
                            : "?";
}

// Disconnect reason (HCI error; NimBLE adds 0x200)
const char* reasonName(int reason) {
  const int r = reason >= 0x200 ? reason - 0x200 : reason;
  switch (r) {
    case 0x05: return "authentication failure";
    case 0x08: return "supervision timeout (radio lost)";
    case 0x13: return "closed by the phone";
    case 0x16: return "closed by this device";
    case 0x1A: return "unsupported remote feature";
    case 0x22: return "response timeout";
    case 0x3B: return "unacceptable connection parameters";
    case 0x3D: return "MIC failure";
    case 0x3E: return "failed to establish";
    default: return "other";
  }
}

class ServerCb : public NimBLEServerCallbacks {
  using NimBLEServerCallbacks::onConnect;
  using NimBLEServerCallbacks::onDisconnect;
  using NimBLEServerCallbacks::onMTUChange;
  void onConnect(NimBLEServer* srv, ble_gap_conn_desc* desc) override {
    s_connected = true;
    s_connects++;
    s_connMs = millis();
    Serial.printf("BLE: connected (%s), interval %.2f ms, latency %u, timeout %u ms\n", NimBLEAddress(desc->peer_ota_addr).toString().c_str(),
                  desc->conn_itvl * 1.25, desc->conn_latency, desc->supervision_timeout * 10);
    if (RC_BLE_CONN_PARAMS) srv->updateConnParams(desc->conn_handle, 6, 12, 0, 400);  // 7.5..15 ms
  }
  void onDisconnect(NimBLEServer*) override {
    s_connected = false;
    s_subscribed = 0;
    Serial.printf("BLE: disconnected after %.1f s, advertising again\n", (millis() - s_connMs) / 1000.0);
  }
  void onMTUChange(uint16_t mtu, ble_gap_conn_desc*) override {
    s_mtu = mtu;
    Serial.printf("BLE: MTU %u bytes\n", mtu);
  }
};

// Every GAP event of interest as one line (disconnect reason, parameter updates, pairing requests)
int gapEvent(ble_gap_event* ev, void*) {
  switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (ev->connect.status != 0) Serial.printf("BLE: connect failed (%d)\n", ev->connect.status);
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      Serial.printf("BLE: disconnect reason: %s (0x%02X)\n", reasonName(ev->disconnect.reason),
                    ev->disconnect.reason >= 0x200 ? ev->disconnect.reason - 0x200 : ev->disconnect.reason);
      break;
    case BLE_GAP_EVENT_CONN_UPDATE: {
      ble_gap_conn_desc d;
      if (ev->conn_update.status == 0 && ble_gap_conn_find(ev->conn_update.conn_handle, &d) == 0) {
        Serial.printf("BLE: connection updated, interval %.2f ms, latency %u, timeout %u ms\n", d.conn_itvl * 1.25, d.conn_latency,
                      d.supervision_timeout * 10);
      } else {
        Serial.printf("BLE: connection update (status %d)\n", ev->conn_update.status);
      }
      break;
    }
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
      if (ev->conn_update_req.peer_params) {
        Serial.printf("BLE: phone asks for interval %.2f..%.2f ms, latency %u, timeout %u ms\n",
                      ev->conn_update_req.peer_params->itvl_min * 1.25, ev->conn_update_req.peer_params->itvl_max * 1.25,
                      ev->conn_update_req.peer_params->latency, ev->conn_update_req.peer_params->supervision_timeout * 10);
      }
      break;
    case BLE_GAP_EVENT_ENC_CHANGE:
      Serial.printf("BLE: encryption change (status %d)\n", ev->enc_change.status);
      break;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
      Serial.printf("BLE: passkey action requested (%u)\n", ev->passkey.params.action);
      break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
      Serial.println("BLE: repeat pairing requested");
      break;
    default:
      break;
  }
  return 0;
}

class FilterCb : public NimBLECharacteristicCallbacks {
  using NimBLECharacteristicCallbacks::onWrite;
  void onWrite(NimBLECharacteristic* ch) override {
    NimBLEAttValue v = ch->getValue();
    const uint8_t* d = v.data();
    const size_t n = v.length();
    lock();
    s_filter.command(d, n);
    const int cnt = s_filter.count();
    unlock();
    if (n >= 1 && d[0] == 0) {
      Serial.println("RaceChrono: CAN filter = deny all");
    } else if (n >= 3 && d[0] == 1) {
      Serial.printf("RaceChrono: CAN filter = allow all (%u ms)\n", static_cast<unsigned>((d[1] << 8) | d[2]));
    } else if (n >= 7 && d[0] == 2) {
      Serial.printf("RaceChrono: allow CAN PID %08lX every %u ms (%d PIDs allowed)\n",
                    static_cast<unsigned long>((static_cast<uint32_t>(d[3]) << 24) | (static_cast<uint32_t>(d[4]) << 16) |
                                               (static_cast<uint32_t>(d[5]) << 8) | d[6]),
                    static_cast<unsigned>((d[1] << 8) | d[2]), cnt);
    } else {
      Serial.printf("RaceChrono: unknown filter command (%u bytes, first 0x%02X)\n", static_cast<unsigned>(n), n ? d[0] : 0);
    }
  }
};

// 0x0005 / 0x0006: nothing is monitored, so no writes are expected; logged when they come
class MonitorCb : public NimBLECharacteristicCallbacks {
  using NimBLECharacteristicCallbacks::onWrite;
  using NimBLECharacteristicCallbacks::onSubscribe;
  void onWrite(NimBLECharacteristic* ch) override {
    NimBLEAttValue v = ch->getValue();
    Serial.printf("BLE: phone wrote %s, %u bytes (first 0x%02X)\n", chName(ch), static_cast<unsigned>(v.length()),
                  v.length() ? v.data()[0] : 0);
  }
  void onSubscribe(NimBLECharacteristic* ch, ble_gap_conn_desc*, uint16_t subValue) override {
    Serial.printf("BLE: phone %s indications of %s\n", subValue ? "enabled" : "disabled", chName(ch));
  }
};

class NotifyCb : public NimBLECharacteristicCallbacks {
  using NimBLECharacteristicCallbacks::onSubscribe;
  using NimBLECharacteristicCallbacks::onRead;
  void onSubscribe(NimBLECharacteristic* ch, ble_gap_conn_desc*, uint16_t subValue) override {
    const uint8_t b = bitOf(ch);
    s_subscribed = static_cast<uint8_t>(subValue ? (s_subscribed | b) : (s_subscribed & ~b));
    Serial.printf("BLE: phone %s notifications of %s\n", subValue ? "subscribed to" : "unsubscribed from", chName(ch));
  }
  void onRead(NimBLECharacteristic* ch) override { Serial.printf("BLE: phone read %s\n", chName(ch)); }
};

void bleTask(void*) {
  RcMsg m;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    for (;;) {  // GPS first, then one CAN at a time (GPS that arrives meanwhile goes next)
      if (xQueueReceive(s_qGps, &m, 0) != pdTRUE && xQueueReceive(s_qCan, &m, 0) != pdTRUE) break;
      NimBLECharacteristic* ch = m.kind == RC_CAN ? s_chCan : m.kind == RC_GPS ? s_chGps : s_chTime;
      ch->setValue(m.data, m.len);  // a read gets the newest value even when nobody is subscribed
      if (!s_connected || ch->getSubscribedCount() == 0) continue;
      ch->notify();
      if (m.kind == RC_CAN) {
        s_canSent++;
      } else if (m.kind == RC_GPS) {
        s_gpsSent++;
      }
    }
  }
}

}  // namespace

bool bleBegin(const char* name) {
  if (s_ok) return true;
  if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
  if (!s_qGps) s_qGps = xQueueCreate(BLE_GPS_QUEUE_LEN, sizeof(RcMsg));
  if (!s_qCan) s_qCan = xQueueCreate(RC_QUEUE_LEN, sizeof(RcMsg));
  if (!s_mutex || !s_qGps || !s_qCan) return false;
  s_filter.reset();
  s_sync.reset();
  static ServerCb srvCb;
  static FilterCb filterCb;
  static NotifyCb notifyCb;
  static MonitorCb monitorCb;
  NimBLEDevice::init(name);
  NimBLEServer* srv = NimBLEDevice::createServer();
  if (!srv) return false;
  srv->setCallbacks(&srvCb, false);
  srv->advertiseOnDisconnect(true);
  NimBLEService* svc = srv->createService(NimBLEUUID(static_cast<uint16_t>(0x1FF8)));
  if (!svc) return false;
  s_chCan = svc->createCharacteristic(NimBLEUUID(static_cast<uint16_t>(0x0001)), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY, 20);
  s_chFilter = svc->createCharacteristic(NimBLEUUID(static_cast<uint16_t>(0x0002)), NIMBLE_PROPERTY::WRITE, 20);
  s_chGps = svc->createCharacteristic(NimBLEUUID(static_cast<uint16_t>(0x0003)), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY, 20);
  s_chTime = svc->createCharacteristic(NimBLEUUID(static_cast<uint16_t>(0x0004)), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY, 3);
  s_chMonCfg = svc->createCharacteristic(NimBLEUUID(static_cast<uint16_t>(0x0005)), NIMBLE_PROPERTY::INDICATE | NIMBLE_PROPERTY::WRITE, 20);
  s_chMonVal = svc->createCharacteristic(NimBLEUUID(static_cast<uint16_t>(0x0006)), NIMBLE_PROPERTY::WRITE_NR, 20);
  if (!s_chCan || !s_chFilter || !s_chGps || !s_chTime || !s_chMonCfg || !s_chMonVal) return false;
  s_chMonCfg->setCallbacks(&monitorCb);
  s_chMonVal->setCallbacks(&monitorCb);
  s_chFilter->setCallbacks(&filterCb);
  s_chCan->setCallbacks(&notifyCb);
  s_chGps->setCallbacks(&notifyCb);
  s_chTime->setCallbacks(&notifyCb);
  static const uint8_t zero[3] = {0, 0, 0};
  s_chTime->setValue(zero, 3);
  if (!svc->start()) return false;
  NimBLEDevice::setCustomGapHandler(gapEvent);
  if (!s_task && xTaskCreatePinnedToCore(bleTask, "ble", 4096, nullptr, 3, &s_task, 0) != pdPASS) return false;
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(svc->getUUID());
  adv->setScanResponse(true);  // the name goes into the scan response (UUID + name do not fit one packet)
  s_ok = adv->start();
  return s_ok;
}

bool bleConnected() { return s_connected; }

void bleSendGps(const lt::GnssFix& f) {
  if (!s_ok) return;
  uint8_t b[20];
  lock();
  const bool sendTime = s_sync.update(f, f.rxMs);  // the date/hour changed, or a second since the last
  const uint8_t sync = s_sync.sync();
  unlock();
  if (sendTime) {
    lt::rcdiy::encodeGpsTime(f, sync, b);
    push(RC_TIME, b, 3);
  }
  lt::rcdiy::encodeGpsMain(f, sync, b);
  push(RC_GPS, b, 20);
}

void bleSendCan(uint32_t id, const uint8_t* data, uint8_t dlc, uint32_t nowMs, uint8_t bus) {
  if (!s_ok) return;
  if (id == lt::rcdiy::PID_IMU_ACCEL || id == lt::rcdiy::PID_IMU_GYRO) {  // the car uses an IMU id
    s_imuIdClash++;
    if (s_imuIdClash == 1 || static_cast<uint32_t>(nowMs - s_imuIdClashMs) >= 60000) {
      s_imuIdClashMs = nowMs;
      Serial.printf("BLE warning: the car sends id %08lX, used for the IMU (%lu times). Change RC_PID_IMU_ACCEL / RC_PID_IMU_GYRO\n",
                    static_cast<unsigned long>(id), static_cast<unsigned long>(s_imuIdClash));
    }
    return;  // not sent, so RaceChrono does not mix it with the IMU values
  }
  if (!s_connected || dlc == 0) return;
  if (bus == 1 && !RC_SEND_BCAN) return;
  if (!filterPass(id, nowMs, lt::rcdiy::RC_CAN_MIN_MS)) return;  // per PID at most every RC_CAN_MIN_MS
  uint8_t b[20];
  push(RC_CAN, b, lt::rcdiy::encodeCan(id, data, dlc, b));
}

void bleSendImu(float lonG, float latG, float vertG, float yawDps, uint32_t nowMs) {
  if (!s_ok) return;
  if (static_cast<int32_t>(nowMs - s_imuSendMs) < 1000 / RC_IMU_HZ) return;
  s_imuSendMs = nowMs;
  if (!s_connected) return;
  uint8_t b[20];
  if (filterPass(lt::rcdiy::PID_IMU_ACCEL, nowMs, 0)) {
    push(RC_CAN, b, lt::rcdiy::encodeImuAccel(lonG, latG, vertG, b));
    s_imuSent++;
  }
  if (filterPass(lt::rcdiy::PID_IMU_GYRO, nowMs, 0)) {
    push(RC_CAN, b, lt::rcdiy::encodeImuGyro(yawDps, b));
    s_imuSent++;
  }
}

void bleStats(BleStats* out) {
  static uint32_t lastMs = 0, canPrev = 0, gpsPrev = 0, imuPrev = 0;
  static uint32_t canPs = 0, gpsPs = 0, imuPs = 0;
  const uint32_t now = millis();
  if (static_cast<uint32_t>(now - lastMs) >= 1000) {  // round gauge rcStatsTick
    lastMs = now;
    const uint32_t c = s_canSent, g = s_gpsSent, i = s_imuSent;
    canPs = c - canPrev;
    canPrev = c;
    gpsPs = g - gpsPrev;
    gpsPrev = g;
    imuPs = i - imuPrev;
    imuPrev = i;
  }
  BleStats st = {};
  st.bleOk = s_ok;
  st.connected = s_connected;
  st.subscribed = s_subscribed;
  st.mtu = s_mtu;
  st.connects = s_connects;
  st.canPerSec = canPs;
  st.gpsPerSec = gpsPs;
  st.imuPerSec = imuPs;
  st.dropped = s_dropped;
  st.imuIdClash = s_imuIdClash;
  if (s_mutex) {
    lock();
    st.imuPidOk = static_cast<uint8_t>((s_filter.allowed(lt::rcdiy::PID_IMU_ACCEL) ? 1 : 0) |
                                       (s_filter.allowed(lt::rcdiy::PID_IMU_GYRO) ? 2 : 0));
    st.filterCount = s_filter.count();
    unlock();
  }
  *out = st;
}

}  // namespace hw
