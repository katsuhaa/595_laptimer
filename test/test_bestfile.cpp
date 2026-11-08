// core/bestfile: the ghost reference lap file (format, checks, trace conversion).
#include <cmath>
#include <cstring>
#include <vector>

#include "common.h"
#include "core/bestfile.h"
#include "core/geo.h"
#include "core/ghost.h"
#include "synth.h"
#include "tmini.h"

namespace {

std::vector<lt::bestfile::Point> samplePoints() {
  return {{353949090, 1400880580, 0}, {353949200, 1400880700, 40}, {353949310, 1400880820, 80}};
}

}  // namespace

TEST(bestfile_crc32_and_layout) {
  const uint8_t check[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  CHECK(lt::bestfile::crc32(0, check, 9) == 0xCBF43926u);
  CHECK(lt::bestfile::crc32(lt::bestfile::crc32(0, check, 4), check + 4, 5) == 0xCBF43926u);  // in pieces
  CHECK(lt::bestfile::fileBytes(0) == 60 && lt::bestfile::fileBytes(2300) == 27660);
  const std::vector<lt::bestfile::Point> pts = samplePoints();
  uint8_t buf[256];
  const int n = lt::bestfile::write(buf, sizeof buf, "sodegaura", 82665, 353940000, 1400890000, pts.data(), 3);
  CHECK(n == lt::bestfile::fileBytes(3));
  CHECK(std::memcmp(buf, "LTB1", 4) == 0 && buf[4] == 1 && buf[5] == 0 && buf[6] == 56 && buf[7] == 0);
  CHECK(std::memcmp(buf + 8, "sodegaura\0\0", 11) == 0 && buf[39] == 0);
  CHECK(buf[40] == (82665 & 0xFF) && buf[41] == ((82665 >> 8) & 0xFF) && buf[42] == (82665 >> 16) && buf[43] == 0);
  CHECK(buf[52] == 3 && buf[53] == 0 && buf[54] == 0 && buf[55] == 0);
  CHECK(buf[56] == (353949090 & 0xFF) && buf[64] == 0 && buf[68] == (353949200 & 0xFF));  // points
  lt::bestfile::Info info{};
  lt::bestfile::Point back[8];
  const char* why = "";
  CHECKF(lt::bestfile::read(buf, n, &info, back, 8, &why), "why %s", why);
  CHECK(info.version == 1 && std::strcmp(info.courseId, "sodegaura") == 0 && info.lapMs == 82665);
  CHECK(info.originLatE7 == 353940000 && info.originLonE7 == 1400890000 && info.numPoints == 3);
  for (int i = 0; i < 3; ++i) {
    CHECK(back[i].latE7 == pts[static_cast<size_t>(i)].latE7 && back[i].lonE7 == pts[static_cast<size_t>(i)].lonE7 &&
          back[i].tMs == pts[static_cast<size_t>(i)].tMs);
  }
  // header only, to size the buffers
  lt::bestfile::Info hi{};
  CHECK(lt::bestfile::readInfo(buf, 56, &hi, &why) && hi.numPoints == 3);
}

TEST(bestfile_every_damage_is_found) {
  const std::vector<lt::bestfile::Point> pts = samplePoints();
  uint8_t good[256];
  const int n = lt::bestfile::write(good, sizeof good, "sodegaura", 82665, 1, -1, pts.data(), 3);
  lt::bestfile::Info info{};
  lt::bestfile::Point back[8];
  int accepted = 0;
  for (int i = 0; i < n; ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      uint8_t b[256];
      std::memcpy(b, good, static_cast<size_t>(n));
      b[i] = static_cast<uint8_t>(b[i] ^ (1u << bit));
      if (lt::bestfile::read(b, n, &info, back, 8, nullptr)) ++accepted;
    }
  }
  CHECKF(accepted == 0, "%d damaged files accepted", accepted);
  const char* why = "";
  CHECK(!lt::bestfile::read(good, n - 1, &info, back, 8, &why) && std::strcmp(why, "bad-size") == 0);
  CHECK(!lt::bestfile::read(good, 40, &info, back, 8, &why) && std::strcmp(why, "short") == 0);
  CHECK(!lt::bestfile::read(good, n, &info, back, 2, &why) && std::strcmp(why, "no-room") == 0);
  uint8_t b[256];
  std::memcpy(b, good, static_cast<size_t>(n));
  b[4] = 2;
  CHECK(!lt::bestfile::read(b, n, &info, back, 8, &why) && std::strcmp(why, "bad-version") == 0);
  std::memcpy(b, good, static_cast<size_t>(n));
  b[0] = 'X';
  CHECK(!lt::bestfile::read(b, n, &info, back, 8, &why) && std::strcmp(why, "bad-magic") == 0);
  // write refuses bad input
  std::vector<lt::bestfile::Point> back2 = pts;
  back2[2].tMs = 10;  // time going back
  CHECK(lt::bestfile::write(b, sizeof b, "sodegaura", 1, 0, 0, back2.data(), 3) == 0);
  CHECK(lt::bestfile::write(b, sizeof b, "", 1, 0, 0, pts.data(), 3) == 0);
  CHECK(lt::bestfile::write(b, sizeof b, "has space", 1, 0, 0, pts.data(), 3) == 0);
  CHECK(lt::bestfile::write(b, sizeof b, "abcdefghijklmnopqrstuvwxyz123456", 1, 0, 0, pts.data(), 3) == 0);  // 32
  CHECK(lt::bestfile::write(b, n - 1, "sodegaura", 1, 0, 0, pts.data(), 3) == 0);
  CHECK(lt::bestfile::write(b, sizeof b, "abcdefghijklmnopqrstuvwxyz12345", 1, 0, 0, pts.data(), 3) == n);  // 31
}

TEST(bestfile_ghost_trace_round_trip) {
  // the trace of one synthetic lap, saved and loaded into another plane, gives the same ghost
  const synth::Track tr;
  const lt::Plane pl = tcommon::plane();
  std::vector<lt::TracePoint> trace;
  const double L = tr.length();
  for (int i = 0; i * 1.2 < L; ++i) {  // 25 Hz at about 30 m/s
    const synth::Pt p = tr.at(i * 1.2);
    trace.push_back({static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(i) * 0.04f});
  }
  const int n = static_cast<int>(trace.size());
  std::vector<uint8_t> file(static_cast<size_t>(lt::bestfile::fileBytes(n)));
  const int bytes = lt::bestfile::writeTrace(file.data(), static_cast<int>(file.size()), "synth", 89120, pl,
                                             trace.data(), n);
  CHECK(bytes == static_cast<int>(file.size()));
  // the same plane: positions within 1 cm, times within 0.5 ms
  std::vector<lt::TracePoint> back(static_cast<size_t>(n));
  lt::bestfile::Info info{};
  const char* why = "";
  CHECKF(lt::bestfile::readTrace(file.data(), bytes, pl, back.data(), n, &info, &why) == n, "why %s", why);
  CHECK(info.originLatE7 == pl.lat0E7() && info.originLonE7 == pl.lon0E7() && info.lapMs == 89120);
  float worstPos = 0.0f, worstT = 0.0f;
  for (int i = 0; i < n; ++i) {
    const lt::TracePoint& a = trace[static_cast<size_t>(i)];
    const lt::TracePoint& b = back[static_cast<size_t>(i)];
    worstPos = std::fmax(worstPos, std::hypot(a.x - b.x, a.y - b.y));
    worstT = std::fmax(worstT, std::fabs(a.tS - b.tS));
  }
  CHECKF(worstPos < 0.01f && worstT <= 0.0005f, "position %.4f m, time %.5f s", static_cast<double>(worstPos),
         static_cast<double>(worstT));
  // another plane (origin 2 km away): the same places
  lt::Plane other;
  other.init(tcommon::kLat0 + 180000, tcommon::kLon0 - 50000);
  CHECK(lt::bestfile::readTrace(file.data(), bytes, other, back.data(), n, &info, &why) == n);
  float worstMoved = 0.0f;
  for (int i = 0; i < n; i += 7) {
    int32_t la = 0, lo = 0;
    other.toGlobal({back[static_cast<size_t>(i)].x, back[static_cast<size_t>(i)].y}, &la, &lo);
    const lt::Vec2 v = pl.toLocal(la, lo);
    worstMoved = std::fmax(worstMoved, std::hypot(v.x - trace[static_cast<size_t>(i)].x, v.y - trace[static_cast<size_t>(i)].y));
  }
  CHECKF(worstMoved < 0.05f, "%.3f m", static_cast<double>(worstMoved));
  // the loaded trace as the ghost's best lap: the delta at the end of an equal lap is 0
  std::vector<lt::TracePoint> a(static_cast<size_t>(n) + 10), b(static_cast<size_t>(n) + 10);
  lt::Ghost g;
  g.begin(a.data(), b.data(), n + 10);
  CHECK(lt::bestfile::readTrace(file.data(), bytes, pl, back.data(), n, &info, &why) == n);
  CHECK(g.setBest(back.data(), n));
  const int64_t t0 = 1000000000;
  g.startLap(t0, {trace[0].x, trace[0].y});
  float d = 99.0f;
  bool any = false;
  for (int i = 1; i < n; ++i) {
    const int64_t t = t0 + static_cast<int64_t>(i) * 40000;
    g.addSample(t, {trace[static_cast<size_t>(i)].x, trace[static_cast<size_t>(i)].y});
    if (g.delta(t, {trace[static_cast<size_t>(i)].x, trace[static_cast<size_t>(i)].y}, &d)) any = true;
  }
  CHECKF(any && std::fabs(d) < 0.002f, "delta %.4f s", static_cast<double>(d));
  // a damaged trace file is refused
  file[100] = static_cast<uint8_t>(file[100] ^ 0x10);
  CHECK(lt::bestfile::readTrace(file.data(), bytes, pl, back.data(), n, &info, &why) == -1 && std::strcmp(why, "bad-crc") == 0);
  CHECK(lt::bestfile::writeTrace(file.data(), bytes - 1, "synth", 1, pl, trace.data(), n) == 0);
}
