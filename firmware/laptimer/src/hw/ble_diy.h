// ble_diy.h - RaceChrono DIY BLE device (github.com/aollin/racechrono-ble-diy-device): ONE device that sends the
// GNSS, the car CAN (from the CAN reader box) and the IMU, exactly like the round gauge (meter_display.ino BLE part,
// NimBLE-Arduino 1.4.3). In RaceChrono: Settings > Add other device > RaceChrono DIY (Bluetooth LE), one entry.
// Service 0x1FF8:
//   0x0001 CAN       READ|NOTIFY  4-byte id (LE) + 1..16 bytes: car CAN and the IMU (RC_PID_IMU_* ids)
//   0x0002 filter    WRITE        RaceChrono writes "deny all" / "allow all" / "allow PID every n ms"
//   0x0003 GPS       READ|NOTIFY  20 bytes per GNSS epoch
//   0x0004 GPS time  READ|NOTIFY  3 bytes (date and hour), tied to 0x0003 by the 3-bit sync
//   0x0005 / 0x0006  monitor characteristics (INDICATE|WRITE / WRITE_NR): only present, nothing is monitored.
//                    Without them the iPhone RaceChrono connected and disconnected in a loop (round gauge 10/15).
// Sending: car CAN per PID at most every RC_CAN_MIN_MS (and only what RaceChrono asked for), B-CAN too
// (RC_SEND_BCAN), IMU at RC_IMU_HZ, GPS time when the hour changes and once per second. A BLE task (core 0) sends
// GPS before any queued CAN (GPS priority); full queues drop (counted), a full GPS queue drops its oldest entry.
// Any task may call the send functions.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "lt_core.h"

namespace hw {

inline constexpr const char* BLE_NAME = "595 laptimer";  // shown in RaceChrono's device list
constexpr int RC_IMU_HZ = 20;            // round gauge: IMU to RaceChrono per second (provisional there)
constexpr int RC_QUEUE_LEN = 48;         // round gauge: notifications waiting (CAN + IMU here)
constexpr int BLE_GPS_QUEUE_LEN = 8;     // GPS main + time notifications waiting
constexpr bool RC_SEND_BCAN = true;      // round gauge: B-CAN raw frames go to RaceChrono too
constexpr bool RC_BLE_CONN_PARAMS = false;  // round gauge: do not ask for a connection interval (phone decides)

struct BleStats {
  bool bleOk;          // bleBegin() succeeded (advertising)
  bool connected;      // a phone is connected
  uint8_t subscribed;  // notifications on: bit0 CAN 0x0001, bit1 GPS 0x0003, bit2 GPS time 0x0004
  uint16_t mtu;        // negotiated ATT MTU (0 = none yet)
  uint32_t connects;   // connections since start
  uint32_t canPerSec;  // CAN notifications (car + IMU) sent in the last full second
  uint32_t gpsPerSec;  // GPS notifications sent in the last full second
  uint32_t imuPerSec;  // IMU payloads queued in the last full second (accel + yaw)
  uint32_t dropped;    // notifications dropped because a queue was full (total)
  uint8_t imuPidOk;    // RaceChrono asked for bit0 RC_PID_IMU_ACCEL, bit1 RC_PID_IMU_GYRO
  int filterCount;     // PIDs allowed by RaceChrono (-1 = all)
  uint32_t imuIdClash;  // car frames that use an IMU PID id (not sent; change RC_PID_IMU_* if this grows)
};

bool bleBegin(const char* name = BLE_NAME);  // NimBLE init, GATT server, advertising, BLE task
bool bleConnected();
void bleSendGps(const lt::GnssFix& f);  // every GNSS epoch (uses f.rxMs as the time)
// One car CAN frame (bus 0 = C-CAN, 1 = B-CAN). Only when connected and RaceChrono asked for the id.
void bleSendCan(uint32_t id, const uint8_t* data, uint8_t dlc, uint32_t nowMs, uint8_t bus = 0);
// Car-frame IMU: [g] forward/right/up with gravity removed, yaw [deg/s] left positive. Call at any rate; it is
// sent at RC_IMU_HZ.
void bleSendImu(float lonG, float latG, float vertG, float yawDps, uint32_t nowMs);
void bleStats(BleStats* out);

}  // namespace hw
