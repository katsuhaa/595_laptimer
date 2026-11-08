// core/meter_link.h - copy of the round gauge meter_display/meter_link.h (katsuhaa/abarth595-round-gauge 05ab0a1), kept identical below this line: change every copy together.
// meter_link.h - XIAO ESP32C6 → 表示器 の UART データ形式  版 2  (2026-10-14)
//
// 版 2 で変えた所: 表示に使う値(MlSignals)にブースト圧 boost_c と、その valid ビット ML_V_BOOST を足した(16 → 18 バイト)。
//
// このファイルは meter_display・c6_display_debug・c6_can_link の 3 か所に、同じ内容で置く。
// 1 つを変えたらほかも同じにすること(METER_LINK_PROTO_VER が違うと、表示器の「受信の状態」に出る)。
//
// 線: XIAO「D6」→ 表示器「IO16」の 1 本(XIAO から表示器への片方向)。8N1。
// 速度: 送る側(XIAO)の METER_LINK_BAUD で決まる。表示器は下の候補を順に試して自動で合わせる。
//
// 1 回分のデータ(フレーム):
//   0xA5 0x5A | 種類 1 | 連番 1 | 長さ 1 | 中身(長さぶん) | CRC 2(下位が先)
//   CRC は CRC-16/CCITT-FALSE(多項式 0x1021、初期値 0xFFFF)。「種類」から「中身」の最後までに掛ける。
//   連番は種類に関係なく 1 ずつ増える(表示器は飛びを数えて、届かなかった数を出す)。
//   数値はすべてリトルエンディアン。
#ifndef METER_LINK_H
#define METER_LINK_H
#include <stdint.h>
#include <string.h>

#define METER_LINK_PROTO_VER 2
#define METER_LINK_BAUD 921600  // 送る側の速度。下げるときは XIAO 側のこの値だけを変える
#define METER_LINK_BAUD_CANDIDATES \
  { 921600, 460800, 230400, 115200 }

#define ML_SYNC0 0xA5
#define ML_SYNC1 0x5A
#define ML_MAX_PAYLOAD 48
#define ML_OVERHEAD 7  // 同期 2 + 種類 1 + 連番 1 + 長さ 1 + CRC 2

enum : uint8_t {
  ML_TYPE_SIGNALS = 0x01,  // 表示に使う値(MlSignals)。50 回/秒
  ML_TYPE_CAN = 0x02,      // CAN の生データ 1 フレーム(MlCanHead + データ)。CAN確認画面用
  ML_TYPE_INFO = 0x03,     // 送る側の状態(MlInfo)。1 回/秒
};

// MlSignals.valid のビット: 1 = その値が使える
enum : uint8_t {
  ML_V_RPM = 0x01,
  ML_V_SPEED = 0x02,
  ML_V_THROTTLE = 0x04,
  ML_V_WATER = 0x08,
  ML_V_FUEL = 0x10,
  ML_V_GEAR = 0x20,
  ML_V_BOOST = 0x40,
};
// MlSignals.flags / MlInfo.flags のビット
enum : uint8_t {
  ML_F_SIM = 0x01,    // 模擬データ(車の値ではない)。表示器は「SIM」を出す
  ML_F_C_CAN = 0x02,  // C-CAN を受信中
  ML_F_B_CAN = 0x04,  // B-CAN を受信中
};

#pragma pack(push, 1)
struct MlSignals {
  uint8_t valid;       // ML_V_*
  uint8_t flags;       // ML_F_*
  uint16_t rpm;        // 回転数 [rpm]
  uint16_t speed_c;    // 車速 [0.01 km/h]
  uint8_t throttle_h;  // アクセル開度 [0.5 %](0〜200)
  uint8_t fuel_h;      // 燃料残量 [0.5 %](0〜200)
  int16_t water_d;     // 水温 [0.1 ℃]
  char gear;           // '1'〜'5'、'N'、'R'。分からないときは 0
  uint8_t scene;       // 模擬データの場面番号(模擬でなければ 0)
  uint32_t t_ms;       // 送る側の起動からの時間 [ms]
  int16_t boost_c;     // ブースト圧(大気圧との差)[0.01 bar]。負は負圧。版 2 で追加
};                     // 18 バイト

struct MlCanHead {
  uint8_t info;  // bit7: バス(0 = C-CAN、1 = B-CAN)、bit6: 29 ビット ID、bit3〜0: データの長さ(0〜8)
  uint32_t id;
};  // このあとにデータ(0〜8 バイト)が続く

struct MlInfo {
  uint8_t proto_ver;    // METER_LINK_PROTO_VER
  uint8_t flags;        // ML_F_*
  uint16_t can_fps[2];  // CAN の受信数 [フレーム/秒]。[0] = C-CAN、[1] = B-CAN
  uint16_t can_ids[2];  // これまでに見た ID の数
  uint32_t can_err[2];  // バスエラーの累計
  uint32_t tx_drop;     // UART が詰まって送れなかったフレームの累計
  uint32_t uptime_s;    // 送る側の起動からの時間 [秒]
  char name[12];        // 送る側のファームの名前(余りは 0)
};                      // 38 バイト
#pragma pack(pop)

static inline uint16_t mlCrc16(const uint8_t *p, size_t n, uint16_t crc = 0xFFFF) {
  while (n--) {
    crc ^= (uint16_t)(*p++) << 8;
    for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

// フレームを out に組み立てる。戻り値はバイト数(out は ML_MAX_PAYLOAD + ML_OVERHEAD 以上)
static inline size_t mlBuild(uint8_t *out, uint8_t type, uint8_t seq, const void *payload, uint8_t len) {
  out[0] = ML_SYNC0;
  out[1] = ML_SYNC1;
  out[2] = type;
  out[3] = seq;
  out[4] = len;
  if (len) memcpy(out + 5, payload, len);
  uint16_t crc = mlCrc16(out + 2, (size_t)len + 3);
  out[5 + len] = (uint8_t)(crc & 0xFF);
  out[6 + len] = (uint8_t)(crc >> 8);
  return (size_t)len + ML_OVERHEAD;
}

#endif  // METER_LINK_H
