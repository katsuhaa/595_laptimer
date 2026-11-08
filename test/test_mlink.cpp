// Tests of core/mlink (meter_link v2 receiver). Ported from the round gauge's pc_test/unit_main.cpp
// sections 1-5, 10 and 11 (katsuhaa/abarth595-round-gauge 05ab0a1) plus info, resync and fuzz checks.
#include <cstring>
#include <vector>

#include "core/meter_link.h"
#include "core/mlink.h"
#include "tmini.h"

namespace {

using lt::mlink::LINK_BAUD_WINDOW_MS;
using lt::mlink::LINK_TIMEOUT_MS;
using lt::mlink::Parser;

std::vector<uint8_t> frame(uint8_t type, uint8_t seq, const void* p, uint8_t len) {
  uint8_t buf[ML_MAX_PAYLOAD + ML_OVERHEAD];
  const size_t n = mlBuild(buf, type, seq, p, len);
  return std::vector<uint8_t>(buf, buf + n);
}

MlSignals sigMsg(uint16_t rpm) {
  MlSignals s;
  std::memset(&s, 0, sizeof(s));
  s.valid = ML_V_RPM | ML_V_SPEED | ML_V_THROTTLE | ML_V_WATER | ML_V_FUEL | ML_V_GEAR | ML_V_BOOST;
  s.rpm = rpm;
  s.speed_c = 6000;
  s.throttle_h = 200;
  s.fuel_h = 124;
  s.water_d = 925;
  s.gear = '3';
  s.boost_c = -45;
  return s;
}

std::vector<uint8_t> canMsg(uint8_t seq, uint8_t bus, bool ext, uint32_t id, const uint8_t* d, uint8_t dlc) {
  uint8_t p[sizeof(MlCanHead) + 8];
  MlCanHead h;
  h.info = static_cast<uint8_t>((bus ? 0x80 : 0) | (ext ? 0x40 : 0) | (dlc & 0x0F));
  h.id = id;
  std::memcpy(p, &h, sizeof(h));
  std::memcpy(p + sizeof(h), d, dlc);
  return frame(ML_TYPE_CAN, seq, p, static_cast<uint8_t>(sizeof(h) + dlc));
}

struct Rec : lt::mlink::Sink {
  int cans = 0, infos = 0, sigs = 0;
  uint8_t bus = 0, ext = 0, dlc = 0;
  uint32_t id = 0, ms = 0;
  uint8_t data[8] = {};
  MlInfo info{};
  MlSignals sig{};
  void onCan(uint8_t b, uint8_t e, uint32_t i, uint8_t n, const uint8_t* d, uint32_t t) override {
    cans++;
    bus = b;
    ext = e;
    id = i;
    dlc = n;
    ms = t;
    std::memcpy(data, d, n);
  }
  void onInfo(const MlInfo& i) override {
    infos++;
    info = i;
  }
  void onSignals(const MlSignals& s) override {
    sigs++;
    sig = s;
  }
};

void feed(Parser& p, const std::vector<uint8_t>& v, uint32_t t) { p.feed(v.data(), v.size(), t); }

}  // namespace

TEST(mlink_value_frame_and_timeout) {
  Rec rec;
  Parser p(&rec);
  const MlSignals m = sigMsg(4200);
  feed(p, frame(ML_TYPE_SIGNALS, 0, &m, sizeof(m)), 1000);
  const uint16_t rpm = rec.sig.rpm;
  const int16_t boost = rec.sig.boost_c;
  CHECK(rec.sigs == 1 && rpm == 4200 && boost == -45 && rec.sig.gear == '3');
  CHECK(p.stats().framesOk == 1 && p.stats().crcErrors == 0 && p.stats().junkBytes == 0);
  lt::mlink::Stats st;
  p.getStats(&st);
  CHECK(st.framesOk == 1 && st.baud == Parser::candidate(0));
  CHECK(p.linkUp(1000) && p.signalsUp(1000) && !p.usingRaw(1000));
  CHECK(p.linkUp(1000 + LINK_TIMEOUT_MS) && p.signalsUp(1000 + LINK_TIMEOUT_MS));  // up to the limit
  CHECK(!p.linkUp(1001 + LINK_TIMEOUT_MS) && !p.signalsUp(1001 + LINK_TIMEOUT_MS));
}

TEST(mlink_byte_by_byte) {
  Rec rec;
  Parser p(&rec);
  const MlSignals m = sigMsg(5100);
  for (uint8_t b : frame(ML_TYPE_SIGNALS, 1, &m, sizeof(m))) p.feed(&b, 1, 2000);
  const uint16_t rpm = rec.sig.rpm;
  CHECK(rec.sigs == 1 && rpm == 5100 && p.stats().framesOk == 1);
}

TEST(mlink_boot_log_and_fake_sync) {
  Rec rec;
  Parser p(&rec);
  std::vector<uint8_t> w;
  const char* boot =
      "ESP-ROM:esp32c6-20220919\r\nBuild:Sep 19 2022\r\nrst:0x1 (POWERON),boot:0xc (SPI_FAST_FLASH_BOOT)\r\n";
  w.insert(w.end(), boot, boot + std::strlen(boot));
  // a frame cut short (the length of a value frame)
  const uint8_t fake[] = {0xA5, 0x5A, 0x01, 0x07, static_cast<uint8_t>(sizeof(MlSignals)), 1, 2, 3};
  w.insert(w.end(), fake, fake + sizeof(fake));
  const MlSignals m = sigMsg(3300);
  const auto f = frame(ML_TYPE_SIGNALS, 2, &m, sizeof(m));
  w.insert(w.end(), f.begin(), f.end());
  const auto f2 = frame(ML_TYPE_SIGNALS, 3, &m, sizeof(m));
  w.insert(w.end(), f2.begin(), f2.end());
  p.feed(w.data(), w.size(), 3000);
  const uint16_t rpm = rec.sig.rpm;
  CHECKF(rpm == 3300 && p.stats().framesOk == 2, "framesOk %u", p.stats().framesOk);
  CHECKF(p.stats().crcErrors == 1, "the cut frame is one CRC error (%u)", p.stats().crcErrors);
  CHECKF(p.stats().junkBytes > 50, "junk %u", p.stats().junkBytes);
  CHECK(p.stats().lostFrames == 0);
}

TEST(mlink_crc_error_and_sequence_gap) {
  Rec rec;
  Parser p(&rec);
  const MlSignals m = sigMsg(2500);
  feed(p, frame(ML_TYPE_SIGNALS, 3, &m, sizeof(m)), 3000);
  auto bad = frame(ML_TYPE_SIGNALS, 4, &m, sizeof(m));
  bad[10] ^= 0x04;  // one bit of the payload
  feed(p, bad, 3100);
  feed(p, frame(ML_TYPE_SIGNALS, 8, &m, sizeof(m)), 3120);  // 4..7 did not arrive
  CHECK(p.stats().crcErrors == 1);
  CHECK(p.stats().framesOk == 2 && rec.sigs == 2);
  CHECKF(p.stats().lostFrames == 4, "lost %u", p.stats().lostFrames);
  // after a pause longer than LINK_TIMEOUT_MS the gap is not counted
  feed(p, frame(ML_TYPE_SIGNALS, 200, &m, sizeof(m)), 3120 + LINK_TIMEOUT_MS + 500);
  CHECK(p.stats().lostFrames == 4);
}

TEST(mlink_can_frames_to_sink) {
  Rec rec;
  Parser p(&rec);
  const uint8_t d[8] = {0, 0x19, 0x10, 0x68, 7, 0x18, 0x46, 0x00};
  feed(p, canMsg(201, 0, true, 0x0618A001, d, 8), 5000);
  CHECK(rec.cans == 1 && rec.bus == 0 && rec.ext == 1 && rec.id == 0x0618A001 && rec.dlc == 8 &&
        rec.ms == 5000 && std::memcmp(rec.data, d, 8) == 0);
  feed(p, canMsg(202, 1, true, 0x04214001, d, 4), 5001);
  CHECK(rec.cans == 2 && rec.bus == 1 && rec.ext == 1 && rec.id == 0x04214001 && rec.dlc == 4);
  feed(p, canMsg(203, 0, false, 0x123, d, 2), 5002);
  CHECK(rec.cans == 3 && rec.ext == 0 && rec.id == 0x123 && rec.dlc == 2);
  CHECK(p.stats().canFrames == 3 && p.linkUp(5002) && p.usingRaw(5002));
  // a CAN frame whose length does not fit its dlc is not used
  uint8_t pl[sizeof(MlCanHead) + 8];
  MlCanHead h;
  h.info = 0x40 | 8;
  h.id = 0x0618A001;
  std::memcpy(pl, &h, sizeof(h));
  feed(p, frame(ML_TYPE_CAN, 204, pl, sizeof(MlCanHead) + 3), 5200);
  CHECK(rec.cans == 3 && p.stats().framesOk == 3 && p.stats().canFrames == 3);
  // dlc above 8 is not used either
  uint8_t pl9[sizeof(MlCanHead) + 9] = {};
  h.info = 0x40 | 9;
  std::memcpy(pl9, &h, sizeof(h));
  feed(p, frame(ML_TYPE_CAN, 205, pl9, sizeof(pl9)), 5300);
  CHECK(rec.cans == 3 && p.stats().framesOk == 3);
}

TEST(mlink_info_frame) {
  Rec rec;
  Parser p(&rec);
  MlInfo info;
  std::memset(&info, 0, sizeof(info));
  info.proto_ver = METER_LINK_PROTO_VER;
  info.flags = ML_F_C_CAN | ML_F_B_CAN;
  info.uptime_s = 77;
  std::memcpy(info.name, "c6_can\x07li\xC3", 10);  // a control char and a non-ASCII byte
  feed(p, frame(ML_TYPE_INFO, 1, &info, sizeof(info)), 1000);
  CHECK(rec.infos == 1 && p.stats().infoValid);
  CHECKF(std::strcmp(p.stats().info.name, "c6_can?li?") == 0, "name '%s'", p.stats().info.name);
  CHECK(std::strcmp(rec.info.name, "c6_can?li?") == 0);
  const uint32_t up = p.stats().info.uptime_s;
  CHECK(p.stats().info.proto_ver == METER_LINK_PROTO_VER && up == 77);
  p.tick(1000);
  CHECK(p.stats().infoValid);
  p.tick(5000);  // more than 3 s after the info frame
  CHECK(!p.stats().infoValid);
}

TEST(mlink_auto_baud) {
  const uint32_t cand[] = METER_LINK_BAUD_CANDIDATES;
  CHECK(Parser::candidates() == 4 && Parser::candidate(0) == cand[0] && Parser::candidate(3) == cand[3]);
  const MlSignals m = sigMsg(3000);
  uint8_t junk[64];
  for (int i = 0; i < 64; i++) junk[i] = static_cast<uint8_t>((i * 37 + 11) | 0x80);  // no A5 5A in it
  uint8_t sq = 0;
  Parser p;
  // (a) nothing arrives: the baud stays
  uint32_t tt = 1000, sw = 0;
  for (int i = 0; i < 2000; i++) sw += p.autoBaud(tt += 5, 0) ? 1 : 0;
  CHECK(sw == 0 && p.baud() == cand[0] && !p.stats().locked && p.stats().baud == cand[0]);
  // (b) XIAO boot log (short junk) -> 0.4 s of silence -> good frames: the baud stays
  p.begin();
  tt = 1000;
  sw = 0;
  for (int i = 0; i < 6; i++) {  // 384 bytes of junk in 30 ms
    p.feed(junk, sizeof(junk), tt);
    sw += p.autoBaud(tt += 5, sizeof(junk)) ? 1 : 0;
  }
  for (int i = 0; i < 80; i++) sw += p.autoBaud(tt += 5, 0) ? 1 : 0;
  for (int i = 0; i < 150; i++) {  // 3 s of frames
    const auto f = frame(ML_TYPE_SIGNALS, sq++, &m, sizeof(m));
    p.feed(f.data(), f.size(), tt);
    for (int k = 0; k < 4; k++) sw += p.autoBaud(tt += 5, k == 0 ? f.size() : 0) ? 1 : 0;
  }
  CHECK(sw == 0 && p.stats().locked && p.stats().baud == cand[0]);
  // (c) junk across a window boundary -> 2 s of silence -> a good frame: the baud stays
  p.begin();
  tt = 1000;
  sw = 0;
  p.autoBaud(tt, 0);  // window starts at 1000
  tt += LINK_BAUD_WINDOW_MS - 15;
  for (int i = 0; i < 6; i++) {
    p.feed(junk, sizeof(junk), tt);
    sw += p.autoBaud(tt += 5, sizeof(junk)) ? 1 : 0;
  }
  for (int i = 0; i < 400; i++) sw += p.autoBaud(tt += 5, 0) ? 1 : 0;
  {
    const auto f = frame(ML_TYPE_SIGNALS, sq++, &m, sizeof(m));
    p.feed(f.data(), f.size(), tt);
    sw += p.autoBaud(tt += 5, f.size()) ? 1 : 0;
  }
  CHECK(sw == 0 && p.stats().locked && p.stats().baud == cand[0]);
  // (d) junk keeps coming (wrong baud): next candidate every 1.5 s, round and back to the first
  p.begin();
  tt = 1000;
  sw = 0;
  uint32_t got[8] = {0}, at[8] = {0};
  for (int i = 0; i < 1300 && sw < 8; i++) {  // 6.5 s
    p.feed(junk, 40, tt);
    const uint32_t nb = p.autoBaud(tt += 5, 40);
    if (nb) {
      got[sw] = nb;
      at[sw] = tt - 1000;
      sw++;
    }
  }
  CHECKF(sw == 4 && got[0] == cand[1] && got[1] == cand[2] && got[2] == cand[3] && got[3] == cand[0],
         "switches %u", sw);
  CHECKF(at[0] >= 1400 && at[0] <= 1600 && at[1] - at[0] >= 1400 && at[1] - at[0] <= 1600,
         "first switch after %u ms, second %u ms later", at[0], at[1] - at[0]);
  CHECK(!p.stats().locked && p.stats().baud == cand[0]);
  // (e) good frames at this baud: it stays, even with junk mixed in
  sw = 0;
  for (int i = 0; i < 300; i++) {  // 6 s, 20 junk bytes per frame
    const auto f = frame(ML_TYPE_SIGNALS, sq++, &m, sizeof(m));
    p.feed(junk, 20, tt);
    p.feed(f.data(), f.size(), tt);
    for (int k = 0; k < 4; k++) sw += p.autoBaud(tt += 5, k == 0 ? f.size() + 20 : 0) ? 1 : 0;
  }
  CHECK(sw == 0 && p.stats().locked && p.stats().baud == cand[0]);
  // (f) the sender stops: not locked, but the baud stays
  for (int i = 0; i < 2000; i++) sw += p.autoBaud(tt += 5, 0) ? 1 : 0;
  CHECK(sw == 0 && !p.stats().locked && p.stats().baud == cand[0]);
}

TEST(mlink_time_skew_grace_and_rates) {
  Parser p;
  const MlSignals m = sigMsg(4100);
  uint8_t sq = 0;
  // a frame stamped after the caller read now (3 ms newer) still counts as up
  feed(p, frame(ML_TYPE_SIGNALS, sq++, &m, sizeof(m)), 7003);
  CHECK(p.linkUp(7000) && p.signalsUp(7000));
  CHECK(!p.linkUp(7003 + LINK_TIMEOUT_MS + 1));
  // after a long gap a large time step must not look "up" again (signed difference wrap)
  for (uint32_t tt = 7003; tt <= 7003 + 4000; tt += 5) p.tick(tt);
  CHECK(!p.linkUp(7003 + 0x7FFFFF00u) && !p.signalsUp(7003 + 0x7FFFFF00u));  // 24 days
  CHECK(!p.linkUp(7003 + 0x80000100u) && !p.signalsUp(7003 + 0x80000100u));  // 25 days
  // grace (flash write): CRC errors and lost frames are not counted; afterwards they are
  uint32_t tt = 20000;
  auto good = [&](uint8_t seqNo) { feed(p, frame(ML_TYPE_SIGNALS, seqNo, &m, sizeof(m)), tt); };
  auto bad = [&](uint8_t seqNo) {
    auto g = frame(ML_TYPE_SIGNALS, seqNo, &m, sizeof(m));
    g[9] ^= 0x10;
    feed(p, g, tt);
  };
  good(10);
  const uint32_t c0 = p.stats().crcErrors, l0 = p.stats().lostFrames;
  p.graceUntil(tt + 300);
  tt += 100;
  bad(11);
  tt += 20;
  good(15);  // 12..14 lost
  CHECK(p.stats().crcErrors == c0 && p.stats().lostFrames == l0);
  tt += 400;
  bad(16);
  tt += 20;
  good(19);  // 17, 18 lost (16 was broken: 3)
  CHECKF(p.stats().crcErrors == c0 + 1 && p.stats().lostFrames == l0 + 3, "crc %u lost %u",
         p.stats().crcErrors - c0, p.stats().lostFrames - l0);
  // bytes per second (frames or not) and frames per second
  p.begin();
  uint8_t junk[50];
  std::memset(junk, 0x33, sizeof(junk));
  p.tick(30000);
  for (int i = 0; i < 10; i++) p.feed(junk, sizeof(junk), 30100 + static_cast<uint32_t>(i) * 50);
  p.tick(31000);
  CHECK(p.stats().bytesPerSec == 500 && p.stats().framesPerSec == 0);
  const uint8_t d[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  for (int i = 0; i < 7; i++) feed(p, canMsg(static_cast<uint8_t>(i), 0, true, 0x100, d, 8), 31100);
  feed(p, frame(ML_TYPE_SIGNALS, 7, &m, sizeof(m)), 31200);
  p.tick(32000);
  CHECK(p.stats().framesPerSec == 8 && p.stats().canPerSec == 7 && p.stats().lostFrames == 0);
}

TEST(mlink_resync_after_source_change) {
  Parser p;
  const MlSignals m = sigMsg(1000);
  for (uint8_t s = 0; s < 5; s++) feed(p, frame(ML_TYPE_SIGNALS, s, &m, sizeof(m)), 1000 + s * 20u);
  // half a frame, then the source changes
  const auto f = frame(ML_TYPE_SIGNALS, 5, &m, sizeof(m));
  p.feed(f.data(), 10, 1100);
  p.resyncSeq();
  feed(p, frame(ML_TYPE_SIGNALS, 120, &m, sizeof(m)), 1120);
  CHECK(p.stats().framesOk == 6 && p.stats().lostFrames == 0 && p.stats().crcErrors == 0);
}

TEST(mlink_random_bytes_between_frames) {
  Rec rec;
  Parser p(&rec);
  uint32_t r = 2463534242u;
  auto rnd = [&]() {
    r ^= r << 13;
    r ^= r >> 17;
    r ^= r << 5;
    return r;
  };
  std::vector<uint8_t> stream;
  int frames = 0;
  uint8_t seq = 0;
  for (int k = 0; k < 3000; k++) {
    const int nj = static_cast<int>(rnd() % 40);
    for (int i = 0; i < nj; i++) stream.push_back(static_cast<uint8_t>(rnd()));
    if (rnd() % 3 == 0) {
      const MlSignals m = sigMsg(static_cast<uint16_t>(rnd() % 8000));
      const auto f = frame(ML_TYPE_SIGNALS, seq++, &m, sizeof(m));
      stream.insert(stream.end(), f.begin(), f.end());
    } else {
      uint8_t d[8];
      for (auto& b : d) b = static_cast<uint8_t>(rnd());
      const auto f = canMsg(seq++, static_cast<uint8_t>(rnd() & 1), true, rnd() & 0x1FFFFFFF, d,
                            static_cast<uint8_t>(rnd() % 9));
      stream.insert(stream.end(), f.begin(), f.end());
    }
    frames++;
  }
  size_t pos = 0;
  uint32_t t = 1000;
  while (pos < stream.size()) {
    size_t n = 1 + rnd() % 300;  // also larger than the 192-byte buffer
    if (n > stream.size() - pos) n = stream.size() - pos;
    p.feed(stream.data() + pos, n, t);
    pos += n;
    t += 1;
  }
  CHECKF(static_cast<int>(p.stats().framesOk) == frames, "framesOk %u of %d", p.stats().framesOk, frames);
  CHECKF(p.stats().lostFrames == 0, "lost %u", p.stats().lostFrames);
  CHECK(rec.sigs + rec.cans == frames);
}
