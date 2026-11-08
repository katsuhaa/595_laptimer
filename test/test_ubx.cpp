// Tests of core/ubx (NEO-M9N UBX). Parser checks ported from the round gauge's pc_test/unit_main.cpp
// section 16 (katsuhaa/abarth595-round-gauge 05ab0a1); the new parts (accuracies, UTC, fix FIFO, epoch
// gaps, NMEA count, MON-VER extensions, RAM-layer VALSET bytes) are checked here with frames built in
// the test and an independent checksum.
#include <cstring>
#include <string>
#include <vector>

#include "core/gtime.h"
#include "core/ubx.h"
#include "tmini.h"

namespace {

void put16(uint8_t* p, int off, uint16_t v) {
  p[off] = static_cast<uint8_t>(v);
  p[off + 1] = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t* p, int off, uint32_t v) {
  for (int i = 0; i < 4; i++) p[off + i] = static_cast<uint8_t>(v >> (8 * i));
}

// Independent UBX frame builder (Fletcher-8 over class..payload)
std::vector<uint8_t> ubxMsg(uint8_t cls, uint8_t id, const uint8_t* pay, size_t len) {
  std::vector<uint8_t> m = {0xB5, 0x62, cls, id, static_cast<uint8_t>(len & 0xFF), static_cast<uint8_t>(len >> 8)};
  m.insert(m.end(), pay, pay + len);
  uint8_t a = 0, b = 0;
  for (size_t i = 2; i < m.size(); i++) {
    a = static_cast<uint8_t>(a + m[i]);
    b = static_cast<uint8_t>(b + a);
  }
  m.push_back(a);
  m.push_back(b);
  return m;
}

bool checksumOk(const uint8_t* m, size_t n) {
  if (n < 8 || m[0] != 0xB5 || m[1] != 0x62) return false;
  const size_t len = static_cast<size_t>(m[4] | (m[5] << 8));
  if (n != len + 8) return false;
  uint8_t a = 0, b = 0;
  for (size_t i = 2; i < n - 2; i++) {
    a = static_cast<uint8_t>(a + m[i]);
    b = static_cast<uint8_t>(b + a);
  }
  return m[n - 2] == a && m[n - 1] == b;
}

struct PvtSpec {
  uint32_t iTOW = 123456789;
  uint16_t year = 2026;
  uint8_t month = 10, day = 14, hour = 3, min = 15, sec = 30;
  uint8_t valid = 0x07;  // validDate | validTime | fullyResolved
  int32_t nano = 250000000;
  uint8_t fixType = 3, flags = 0x01, numSV = 11;
  int32_t lon = 1396821234, lat = 355812345, hMSL = 45000;
  uint32_t hAcc = 1234, vAcc = 2345;
  int32_t gSpeed = 13417, headMot = 12345000;  // 48.30 km/h, 123.45 deg
  uint32_t sAcc = 210, headAcc = 456789;
  uint16_t pDOP = 150;
};

std::vector<uint8_t> pvtMsg(const PvtSpec& s) {
  uint8_t pay[92];
  std::memset(pay, 0, sizeof(pay));
  put32(pay, 0, s.iTOW);
  put16(pay, 4, s.year);
  pay[6] = s.month;
  pay[7] = s.day;
  pay[8] = s.hour;
  pay[9] = s.min;
  pay[10] = s.sec;
  pay[11] = s.valid;
  put32(pay, 12, 25);  // tAcc
  put32(pay, 16, static_cast<uint32_t>(s.nano));
  pay[20] = s.fixType;
  pay[21] = s.flags;
  pay[23] = s.numSV;
  put32(pay, 24, static_cast<uint32_t>(s.lon));
  put32(pay, 28, static_cast<uint32_t>(s.lat));
  put32(pay, 32, static_cast<uint32_t>(s.hMSL + 39000));  // height above ellipsoid (not used)
  put32(pay, 36, static_cast<uint32_t>(s.hMSL));
  put32(pay, 40, s.hAcc);
  put32(pay, 44, s.vAcc);
  put32(pay, 60, static_cast<uint32_t>(s.gSpeed));
  put32(pay, 64, static_cast<uint32_t>(s.headMot));
  put32(pay, 68, s.sAcc);
  put32(pay, 72, s.headAcc);
  put16(pay, 76, s.pDOP);
  return ubxMsg(0x01, 0x07, pay, sizeof(pay));
}

std::vector<uint8_t> ackMsg(bool ack, uint8_t cls, uint8_t id) {
  const uint8_t p[2] = {cls, id};
  return ubxMsg(0x05, ack ? 0x01 : 0x00, p, 2);
}

std::string nmea(const char* body) {  // "$" + body + "*HH\r\n" with the right checksum
  uint8_t x = 0;
  for (const char* c = body; *c; c++) x = static_cast<uint8_t>(x ^ static_cast<uint8_t>(*c));
  char tail[8];
  std::snprintf(tail, sizeof(tail), "*%02X\r\n", x);
  return std::string("$") + body + tail;
}

void feedAll(lt::ubx::Parser& p, const std::vector<uint8_t>& v, uint32_t t) { p.feed(v.data(), v.size(), t); }

// key/value pairs of a CFG-VALSET frame
struct Kv {
  uint32_t key;
  uint32_t val;
  int size;
};
std::vector<Kv> valsetKeys(const uint8_t* m, size_t n, bool* ok) {
  std::vector<Kv> out;
  size_t pos = 10;  // B5 62 06 8A len(2) + version, layers, reserved(2)
  *ok = true;
  while (pos + 4 <= n - 2) {
    const uint32_t key = static_cast<uint32_t>(m[pos]) | (static_cast<uint32_t>(m[pos + 1]) << 8) |
                         (static_cast<uint32_t>(m[pos + 2]) << 16) | (static_cast<uint32_t>(m[pos + 3]) << 24);
    const int sz = static_cast<int>((key >> 28) & 7);
    const int vn = sz == 3 ? 2 : sz == 4 ? 4 : sz == 5 ? 8 : 1;
    if (pos + 4 + static_cast<size_t>(vn) > n - 2) {
      *ok = false;
      break;
    }
    uint32_t v = 0;
    for (int i = 0; i < vn && i < 4; i++) v |= static_cast<uint32_t>(m[pos + 4 + i]) << (8 * i);
    out.push_back({key, v, vn});
    pos += 4 + static_cast<size_t>(vn);
  }
  if (pos != n - 2) *ok = false;
  return out;
}

}  // namespace

TEST(ubx_navpvt_fields_bytewise) {
  lt::ubx::Parser p;
  const PvtSpec s;
  const auto msg = pvtMsg(s);
  int got = 0;
  for (size_t i = 0; i < msg.size(); i++) got += p.feed(&msg[i], 1, 5000);
  lt::GnssFix f;
  CHECK(got == 1 && p.counters().pvt == 1 && p.counters().frames == 1 && p.latest(&f));
  CHECK(f.fixOk && f.fixType == 3 && f.numSV == 11 && !f.diffSoln);
  CHECK(f.timeOk && f.timeResolved && f.year == 2026 && f.month == 10 && f.day == 14 && f.hour == 3 &&
        f.min == 15 && f.sec == 30 && f.nano == 250000000);
  CHECK(f.lat == 355812345 && f.lon == 1396821234 && f.hMSL == 45000 && f.gSpeed == 13417 &&
        f.headMot == 12345000 && f.pDOP == 150 && f.rxMs == 5000 && f.iTOW == 123456789);
  CHECK(f.hAcc_mm == 1234 && f.vAcc_mm == 2345 && f.sAcc_mmps == 210 && f.headAcc == 456789);
  const int64_t utc = static_cast<int64_t>(lt::gtime::epoch(2026, 10, 14, 3, 15, 30)) * 1000000 + 250000;
  CHECKF(f.utcUs == utc, "utcUs %lld", static_cast<long long>(f.utcUs));
  lt::GnssFix q;
  CHECK(p.takeFix(&q) && q.utcUs == utc && !p.takeFix(&q));
}

TEST(ubx_navpvt_flags_and_time_variants) {
  lt::ubx::Parser p;
  PvtSpec s;
  s.nano = -200000;  // -0.2 ms: UTC is just before the second
  s.flags = 0x03;    // gnssFixOK | diffSoln
  feedAll(p, pvtMsg(s), 1);
  lt::GnssFix f;
  CHECK(p.latest(&f) && f.fixOk && f.diffSoln);
  CHECK(f.utcUs == static_cast<int64_t>(lt::gtime::epoch(2026, 10, 14, 3, 15, 30)) * 1000000 - 200);
  s.valid = 0x01;  // date only
  feedAll(p, pvtMsg(s), 2);
  CHECK(p.latest(&f) && !f.timeOk && f.utcUs == 0);
  s.valid = 0x03;  // not fully resolved
  feedAll(p, pvtMsg(s), 3);
  CHECK(p.latest(&f) && f.timeOk && !f.timeResolved);
  s.flags = 0x00;  // no gnssFixOK
  feedAll(p, pvtMsg(s), 4);
  CHECK(p.latest(&f) && !f.fixOk);
  s.flags = 0x01;
  s.fixType = 5;  // time only
  feedAll(p, pvtMsg(s), 5);
  CHECK(p.latest(&f) && !f.fixOk);
  s.fixType = 2;
  feedAll(p, pvtMsg(s), 6);
  CHECK(p.latest(&f) && f.fixOk && f.fixType == 2);
}

TEST(ubx_two_pvt_ack_nak) {
  lt::ubx::Parser p;
  const auto msg = pvtMsg(PvtSpec());
  std::vector<uint8_t> two(msg);
  two.insert(two.end(), msg.begin(), msg.end());
  for (auto m : {ackMsg(true, 0x06, 0x8A), ackMsg(false, 0x06, 0x8A), ackMsg(true, 0x06, 0x01)}) {
    two.insert(two.end(), m.begin(), m.end());
  }
  const int got = p.feed(two.data(), two.size(), 6000);
  const lt::ubx::Counters& c = p.counters();
  CHECK(got == 2 && c.pvt == 2 && c.ack == 2 && c.nak == 1 && c.bad == 0 && c.frames == 5);
  CHECK(c.ackValset == 1 && c.nakValset == 1);  // ACK of CFG-MSG (06 01) is not a VALSET ACK
}

TEST(ubx_checksum_error_and_long_messages) {
  lt::ubx::Parser p;
  const auto msg = pvtMsg(PvtSpec());
  std::vector<uint8_t> bad(msg);
  bad[30] ^= 0x01;
  int got = p.feed(bad.data(), bad.size(), 7000);
  CHECK(got == 0 && p.counters().bad == 1 && p.counters().pvt == 0);
  // a 600-byte message is skipped, the NAV-PVT after it is used
  std::vector<uint8_t> pay600(600);
  for (int i = 0; i < 600; i++) pay600[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
  std::vector<uint8_t> lng = ubxMsg(0x02, 0x15, pay600.data(), pay600.size());
  lng.insert(lng.end(), msg.begin(), msg.end());
  got = p.feed(lng.data(), lng.size(), 8000);
  CHECK(got == 1 && p.counters().pvt == 1 && p.counters().bad == 1 && p.counters().frames == 2);
  // an impossible length (over 1024) resyncs at once and the next NAV-PVT is used
  std::vector<uint8_t> huge = {0xB5, 0x62, 0x02, 0x13, 0x01, 0x08};  // 2049 bytes
  huge.insert(huge.end(), msg.begin(), msg.end());
  got = p.feed(huge.data(), huge.size(), 8100);
  CHECK(got == 1 && p.counters().pvt == 2);
  // NAV-PVT with a wrong length is not decoded
  uint8_t pay90[90] = {};
  const auto shortPvt = ubxMsg(0x01, 0x07, pay90, sizeof(pay90));
  got = p.feed(shortPvt.data(), shortPvt.size(), 8200);
  CHECK(got == 0 && p.counters().pvt == 2 && p.counters().frames == 4);
}

TEST(ubx_nmea_and_junk) {
  lt::ubx::Parser p;
  const auto msg = pvtMsg(PvtSpec());
  const std::string gga = nmea("GNGGA,031530.00,3534.87,N,13940.93,E,1,11,1.5,45.0,M,39.0,M,,");
  const std::string rmc = nmea("GNRMC,031530.00,A,3534.87,N,13940.93,E,26.1,123.4,141026,,,A");
  std::string badCk = nmea("GNGSA,A,3,01,02,03,,,,,,,,,,1.6,1.0,1.2,1");
  badCk[badCk.size() - 3] = badCk[badCk.size() - 3] == '0' ? '1' : '0';  // wrong checksum digit
  std::vector<uint8_t> junk(gga.begin(), gga.end());
  junk.push_back(0xB5);  // false sync
  junk.push_back(0x11);
  junk.insert(junk.end(), msg.begin(), msg.end());
  junk.insert(junk.end(), badCk.begin(), badCk.end());
  junk.insert(junk.end(), rmc.begin(), rmc.end());
  const char* broken = "$GNGLL,3534.87,N*7";  // cut short
  junk.insert(junk.end(), broken, broken + std::strlen(broken));
  junk.insert(junk.end(), rmc.begin(), rmc.end());
  const char* lower = "$gngll,1*00\r\n";  // not an NMEA address
  junk.insert(junk.end(), lower, lower + std::strlen(lower));
  const int got = p.feed(junk.data(), junk.size(), 9000);
  CHECK(got == 1 && p.counters().pvt == 1);
  CHECKF(p.counters().nmea == 3, "nmea %u", p.counters().nmea);
  CHECK(p.counters().bytes == junk.size());
}

TEST(ubx_fix_fifo_and_overflow) {
  lt::ubx::Parser p;
  PvtSpec s;
  std::vector<uint8_t> all;
  for (int i = 0; i < 10; i++) {
    s.iTOW = 1000000 + static_cast<uint32_t>(i) * 40;
    const auto m = pvtMsg(s);
    all.insert(all.end(), m.begin(), m.end());
  }
  CHECK(p.feed(all.data(), all.size(), 100) == 10);
  CHECK(p.counters().fixDropped == 2);  // the queue keeps the newest 8
  lt::GnssFix f;
  int n = 0;
  uint32_t prev = 0;
  bool ordered = true;
  while (p.takeFix(&f)) {
    if (n == 0 && f.iTOW != 1000000 + 2 * 40) ordered = false;
    if (n > 0 && f.iTOW != prev + 40) ordered = false;
    prev = f.iTOW;
    n++;
  }
  CHECK(n == 8 && ordered && prev == 1000000 + 9 * 40);
  CHECK(p.latest(&f) && f.iTOW == 1000000 + 9 * 40);
  // one at a time: nothing dropped
  for (int i = 10; i < 30; i++) {
    s.iTOW = 1000000 + static_cast<uint32_t>(i) * 40;
    feedAll(p, pvtMsg(s), 200 + static_cast<uint32_t>(i));
    CHECK(p.takeFix(&f) && f.iTOW == s.iTOW && !p.takeFix(&f));
  }
  CHECK(p.counters().fixDropped == 2);
}

TEST(ubx_epoch_gaps) {
  lt::ubx::Parser p;  // period 0: no counting
  PvtSpec s;
  uint32_t t = 1000;
  auto epoch = [&](uint32_t itow, uint32_t rx) {
    s.iTOW = itow;
    feedAll(p, pvtMsg(s), rx);
  };
  epoch(500000, t);
  epoch(501000, t += 1000);  // 1 Hz before the configuration
  CHECK(p.counters().missedEpochs == 0 && p.counters().gaps == 0);
  p.setEpochPeriodMs(40);
  CHECK(p.epochPeriodMs() == 40);
  uint32_t it = 600000;
  epoch(it, t += 40);  // reference only
  for (int i = 0; i < 25; i++) epoch(it += 40, t += 40);
  CHECK(p.counters().missedEpochs == 0 && p.counters().gaps == 0 && p.counters().maxGapMs == 40);
  epoch(it += 80, t += 80);  // 1 missing
  epoch(it += 40, t += 40);
  epoch(it += 200, t += 200);  // 4 missing
  CHECKF(p.counters().missedEpochs == 5 && p.counters().gaps == 2 && p.counters().maxGapMs == 200,
         "missed %u gaps %u max %u", p.counters().missedEpochs, p.counters().gaps, p.counters().maxGapMs);
  epoch(it += 42, t += 55);  // jitter of the arrival time is no gap
  epoch(it += 38, t += 25);
  CHECK(p.counters().missedEpochs == 5);
  epoch(it, t += 3);  // the same epoch twice
  CHECK(p.counters().dupEpochs == 1 && p.counters().missedEpochs == 5);
  // week wrap: 604799960 -> 0 is one period
  epoch(604799960, t += 40);  // iTOW jumps: disagrees with the local clock -> local time used
  CHECK(p.counters().timeJumps == 1 && p.counters().missedEpochs == 5);
  epoch(0, t += 40);
  epoch(40, t += 40);
  CHECKF(p.counters().missedEpochs == 5 && p.counters().timeJumps == 1, "missed %u jumps %u",
         p.counters().missedEpochs, p.counters().timeJumps);
  // a 3-second outage (receiver silent): 74 epochs missing
  epoch(3040, t += 3000);
  CHECKF(p.counters().missedEpochs == 5 + 74 && p.counters().gaps == 3, "missed %u", p.counters().missedEpochs);
  // receiver time jumps while it keeps sending: counted from the local clock
  epoch(400000000, t += 40);
  CHECK(p.counters().timeJumps == 2 && p.counters().missedEpochs == 79);
  // setting the period again restarts the reference
  p.setEpochPeriodMs(40);
  epoch(500000000, t += 40);
  epoch(500000040, t += 40);
  CHECK(p.counters().missedEpochs == 79 && p.counters().timeJumps == 2);
  p.begin();  // keeps the period
  CHECK(p.epochPeriodMs() == 40 && p.counters().missedEpochs == 0);
}

TEST(ubx_monver_strings) {
  lt::ubx::Parser p;
  uint8_t pay[40 + 30 * 5];
  std::memset(pay, 0, sizeof(pay));
  std::memcpy(pay, "EXT CORE 4.04 (7f89f7)", 22);
  std::memcpy(pay + 30, "00190000", 8);
  const char* ext[5] = {"ROM BASE 0x118B2060", "FWVER=SPG 4.04", "PROTVER=32.01", "MOD=NEO-M9N", "GPS;GLO;GAL;BDS"};
  for (int i = 0; i < 5; i++) std::memcpy(pay + 40 + 30 * i, ext[i], std::strlen(ext[i]));
  char sw[32], hw[12], mod[32];
  CHECK(!p.monVer(sw, sizeof(sw), hw, sizeof(hw)));
  feedAll(p, ubxMsg(0x0A, 0x04, pay, sizeof(pay)), 100);
  CHECK(p.counters().monVer == 1 && p.monVer(sw, sizeof(sw), hw, sizeof(hw)));
  CHECK(std::strcmp(sw, "EXT CORE 4.04 (7f89f7)") == 0 && std::strcmp(hw, "00190000") == 0);
  CHECK(p.monVerExtCount() == 5 && p.monVerExt(3, mod, sizeof(mod)) && std::strcmp(mod, "MOD=NEO-M9N") == 0);
  CHECK(p.monVerFind("MOD=", mod, sizeof(mod)) && std::strcmp(mod, "NEO-M9N") == 0);
  CHECK(p.monVerFind("FWVER=", mod, sizeof(mod)) && std::strcmp(mod, "SPG 4.04") == 0);
  CHECK(!p.monVerFind("XYZ=", mod, sizeof(mod)) && !p.monVerExt(5, mod, sizeof(mod)));
  char tiny[4];
  CHECK(p.monVer(tiny, sizeof(tiny), nullptr, 0) && std::strcmp(tiny, "EXT") == 0);
  // 30 characters without a terminating NUL are kept whole
  std::memset(pay, 'A', 30);
  feedAll(p, ubxMsg(0x0A, 0x04, pay, 40), 200);
  CHECK(p.monVer(sw, sizeof(sw), hw, sizeof(hw)) && std::strlen(sw) == 30 && p.monVerExtCount() == 0);
  // more extensions than kept
  uint8_t big[40 + 30 * 7];
  std::memset(big, 0, sizeof(big));
  for (int i = 0; i < 7; i++) big[40 + 30 * i] = static_cast<uint8_t>('a' + i);
  feedAll(p, ubxMsg(0x0A, 0x04, big, sizeof(big)), 300);
  CHECK(p.monVerExtCount() == lt::ubx::kMonVerExtMax && p.counters().monVer == 3);
}

TEST(ubx_valset_nav_config_bytes) {
  uint8_t cfg[128];
  const size_t n = lt::ubx::buildNavConfig(cfg, sizeof(cfg), 25);
  CHECKF(n == 49, "size %zu", n);
  CHECK(cfg[0] == 0xB5 && cfg[1] == 0x62 && cfg[2] == 0x06 && cfg[3] == 0x8A && n == static_cast<size_t>(cfg[4] | (cfg[5] << 8)) + 8);
  CHECK(cfg[6] == 0x00 && cfg[7] == 0x01 && cfg[8] == 0 && cfg[9] == 0);  // version 0, RAM layer only
  CHECK(checksumOk(cfg, n));
  bool ok = false;
  const std::vector<Kv> kv = valsetKeys(cfg, n, &ok);
  CHECK(ok && kv.size() == 7);
  const Kv want[7] = {{0x30210001, 40, 2}, {0x30210002, 1, 2}, {0x20910007, 1, 1}, {0x10740001, 1, 1},
                      {0x10740002, 0, 1},  {0x10730001, 1, 1}, {0x20110021, 4, 1}};
  for (size_t i = 0; i < 7 && i < kv.size(); i++) {
    CHECKF(kv[i].key == want[i].key && kv[i].val == want[i].val && kv[i].size == want[i].size,
           "key %zu: %08X = %u (%d bytes)", i, kv[i].key, kv[i].val, kv[i].size);
  }
  // other rates: 10 Hz = 100 ms, above 25 Hz clamps to 40 ms, 0 = 1 s
  const int rates[3][2] = {{10, 100}, {50, 40}, {0, 1000}};
  for (const auto& r : rates) {
    const size_t m = lt::ubx::buildNavConfig(cfg, sizeof(cfg), r[0]);
    const std::vector<Kv> k = valsetKeys(cfg, m, &ok);
    CHECKF(m == 49 && ok && !k.empty() && k[0].key == 0x30210001 && k[0].val == static_cast<uint32_t>(r[1]) &&
               checksumOk(cfg, m),
           "rate %d Hz", r[0]);
  }
  CHECK(lt::ubx::buildNavConfig(cfg, 48, 25) == 0 && lt::ubx::buildNavConfig(nullptr, 100, 25) == 0);
}

TEST(ubx_valset_baud_bytes) {
  uint8_t m[32];
  size_t n = lt::ubx::buildBaud(m, sizeof(m), 230400);
  // B5 62 06 8A 0C 00 | 00 01 00 00 | 01 00 52 40 | 00 84 03 00 | ckA ckB
  const uint8_t want[18] = {0xB5, 0x62, 0x06, 0x8A, 0x0C, 0x00, 0x00, 0x01, 0x00,
                            0x00, 0x01, 0x00, 0x52, 0x40, 0x00, 0x84, 0x03, 0x00};
  CHECK(n == 20 && std::memcmp(m, want, sizeof(want)) == 0 && checksumOk(m, n));
  n = lt::ubx::buildBaud(m, sizeof(m), 38400);
  CHECK(n == 20 && m[14] == 0x00 && m[15] == 0x96 && m[16] == 0x00 && m[17] == 0x00 && checksumOk(m, n));
  CHECK(lt::ubx::buildBaud(m, 19, 230400) == 0);
}

TEST(ubx_poll_monver_known_bytes) {
  uint8_t m[16];
  const size_t n = lt::ubx::buildPollMonVer(m, sizeof(m));
  const uint8_t want[8] = {0xB5, 0x62, 0x0A, 0x04, 0x00, 0x00, 0x0E, 0x34};  // the well-known poll
  CHECK(n == 8 && std::memcmp(m, want, 8) == 0);
  CHECK(lt::ubx::buildPollMonVer(m, 7) == 0);
  // the builder makes frames the parser accepts
  lt::ubx::Parser p;
  uint8_t cfg[64];
  const size_t k = lt::ubx::buildNavConfig(cfg, sizeof(cfg), 25);
  p.feed(cfg, k, 1);
  p.feed(m, n, 2);
  CHECK(p.counters().frames == 2 && p.counters().bad == 0);
}

TEST(ubx_to_lap_fix) {
  lt::ubx::Parser p;
  PvtSpec s;
  feedAll(p, pvtMsg(s), 10);
  lt::GnssFix g;
  CHECK(p.latest(&g));
  lt::Fix f = lt::toLapFix(g);
  CHECK(f.valid && f.tUs == g.utcUs && f.latE7 == s.lat && f.lonE7 == s.lon);
  CHECKF(f.speedMps > 13.416f && f.speedMps < 13.418f, "speed %.4f", static_cast<double>(f.speedMps));
  lt::LapFixRules r;
  r.maxHAccMm = 1000;  // 1.234 m is worse
  CHECK(!lt::toLapFix(g, r).valid);
  r.maxHAccMm = 2000;
  CHECK(lt::toLapFix(g, r).valid);
  g.fixType = 2;
  CHECK(!lt::toLapFix(g).valid);
  r.minFixType = 2;
  CHECK(lt::toLapFix(g, r).valid);
  g.fixType = 3;
  g.timeOk = false;
  CHECK(!lt::toLapFix(g).valid);
  g.timeOk = true;
  g.fixOk = false;
  CHECK(!lt::toLapFix(g).valid);
  // consecutive epochs are 40 ms apart in tUs
  s.sec = 31;
  s.nano = 290000000;
  s.iTOW += 1040;
  feedAll(p, pvtMsg(s), 50);
  lt::GnssFix g2;
  CHECK(p.latest(&g2) && lt::toLapFix(g2).tUs - lt::toLapFix(g, r).tUs == 1040000);
}

TEST(ubx_tick_rates) {
  lt::ubx::Parser p(40);
  PvtSpec s;
  uint32_t t = 0;
  p.tick(t);
  for (int i = 0; i < 25; i++) {
    s.iTOW += 40;
    feedAll(p, pvtMsg(s), t += 40);
    p.tick(t);
  }
  p.tick(t = 1000);
  CHECKF(p.counters().pvtPerSec == 25 && p.counters().bytesPerSec == 25 * 100, "pvt/s %u bytes/s %u",
         p.counters().pvtPerSec, p.counters().bytesPerSec);
  p.tick(2000);
  CHECK(p.counters().pvtPerSec == 0);
}

TEST(ubx_random_bytes_do_not_break_the_parser) {
  lt::ubx::Parser p(40);
  uint32_t r = 88172645u;
  auto rnd = [&]() {
    r ^= r << 13;
    r ^= r >> 17;
    r ^= r << 5;
    return r;
  };
  // junk without 0xB5: every NAV-PVT is found
  std::vector<uint8_t> stream;
  PvtSpec s;
  int frames = 0;
  for (int k = 0; k < 2000; k++) {
    const int nj = static_cast<int>(rnd() % 30);
    for (int i = 0; i < nj; i++) {
      uint8_t b = static_cast<uint8_t>(rnd());
      if (b == 0xB5) b = 0x35;
      stream.push_back(b);
    }
    s.iTOW += 40;
    const auto m = pvtMsg(s);
    stream.insert(stream.end(), m.begin(), m.end());
    frames++;
  }
  size_t pos = 0;
  uint32_t t = 0;
  int got = 0;
  lt::GnssFix f;
  while (pos < stream.size()) {
    size_t n = 1 + rnd() % 200;
    if (n > stream.size() - pos) n = stream.size() - pos;
    got += p.feed(stream.data() + pos, n, t += 7);
    pos += n;
    while (p.takeFix(&f)) {
    }
  }
  CHECKF(got == frames && p.counters().pvt == static_cast<uint32_t>(frames), "got %d of %d", got, frames);
  // pure random bytes (with sync bytes): no crash, only counters move
  lt::ubx::Parser q(40);
  std::vector<uint8_t> noise(200000);
  for (auto& b : noise) b = static_cast<uint8_t>(rnd());
  for (size_t i = 0; i < 50; i++) {
    noise[i * 4000] = 0xB5;
    noise[i * 4000 + 1] = 0x62;
  }
  q.feed(noise.data(), noise.size(), 1);
  CHECK(q.counters().bytes == noise.size() && q.counters().pvt <= 1);
}
