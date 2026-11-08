// test/test_reportdata.cpp - core/reportdata: the report decimator, the main log -> report CSV
// filter (any input/output chunking), script-block escaping, the info block, and the web
// module's formatting helpers (names, file kinds, HTTP dates, JSON/XML, PROPFIND entries).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/reportdata.h"
#include "tmini.h"

namespace {

using lt::report::Decimator;
using lt::report::FilterStats;
using lt::report::LogFilter;

const char kArchHeader[] =
    "utc_ms,lat,lon,alt_m,speed_kmh,heading_deg,hacc_m,sats,fix,lon_g,lat_g,vert_g,yaw_dps,rpm,"
    "throttle_pct,speed_can_kmh,water_c,fuel_pct,boost_bar,gear,lap,sector,lap_ms,ghost_delta_s";
const char kReportHeader[] =
    "utc_ms,lat,lon,speed_kmh,lon_g,lat_g,rpm,throttle_pct,boost_bar,water_c,fuel_pct,lap,sector,lap_ms";

uint32_t g_rng = 0x595A1234u;
uint32_t rnd() {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return g_rng;
}

// One main-log line with the ARCH 5.2 formatting, values that look like a lap at Sodegaura
std::string mainLine(int64_t utcMs, int i) {
  const double a = i * 0.002;
  const double lat = 35.3940000 + 0.0020 * std::sin(a);
  const double lon = 140.0890000 + 0.0025 * std::cos(a);
  const double kmh = 120.0 + 50.0 * std::sin(a * 3.0);
  const int lap = 1 + i / 2200;
  char b[400];
  std::snprintf(b, sizeof b,
                "%lld,%.7f,%.7f,%.1f,%.2f,%.1f,%.2f,%d,%d,%.3f,%.3f,%.3f,%.1f,%d,%.1f,%.2f,%.1f,%.1f,%.2f,%c,"
                "%d,%d,%d,%.3f\n",
                static_cast<long long>(utcMs), lat, lon, 95.3, kmh, 214.5, 0.42, 17, 3,
                0.31 * std::sin(a * 5.0), -1.05 * std::cos(a * 4.0), 0.012, 21.5,
                4000 + static_cast<int>(2400.0 * std::sin(a * 3.0)), 87.5, kmh - 1.2, 91.5, 62.0,
                1.12 * std::fabs(std::sin(a * 3.0)), '4', lap, 1 + (i / 500) % 4, (i % 2200) * 40,
                -0.123 + 0.001 * (i % 100));
  return b;
}

std::string mainLog(int rows, int stepMs) {
  std::string s = std::string(kArchHeader) + "\n";
  const int64_t t0 = 1794101400000LL;
  for (int i = 0; i < rows; ++i) s += mainLine(t0 + static_cast<int64_t>(i) * stepMs, i);
  return s;
}

// Runs the filter with input chunks of chunk(i) bytes and an output buffer of outCap bytes
std::string runFilter(const std::string& in, int lineCap, int outCap, int fixedChunk, FilterStats* st) {
  std::vector<char> line(static_cast<size_t>(lineCap));
  std::vector<char> out(static_cast<size_t>(outCap));
  LogFilter f;
  f.begin(line.data(), lineCap);
  std::string res;
  size_t pos = 0;
  int used = 0;
  int guard = 0;
  while (pos < in.size()) {
    int chunk = fixedChunk > 0 ? fixedChunk : static_cast<int>(1 + rnd() % 900);
    if (static_cast<size_t>(chunk) > in.size() - pos) chunk = static_cast<int>(in.size() - pos);
    int off = 0;
    while (off < chunk) {
      int produced = 0;
      const int k = f.feed(in.data() + pos + off, chunk - off, out.data() + used, outCap - used, &produced);
      used += produced;
      off += k;
      if (off < chunk) {  // output full: send it
        res.append(out.data(), static_cast<size_t>(used));
        if (used == 0 && ++guard > 10) return "STUCK";
        used = 0;
      }
    }
    pos += static_cast<size_t>(chunk);
  }
  f.finish();
  res.append(out.data(), static_cast<size_t>(used));
  *st = f.stats();
  return res;
}

int countLines(const std::string& s) {
  int n = 0;
  for (char c : s) n += c == '\n' ? 1 : 0;
  return n;
}

}  // namespace

TEST(reportdata_decimator_rates) {
  struct Case {
    int stepMs;
    int keepEvery;
  } cases[] = {{40, 3}, {50, 2}, {100, 1}, {20, 5}};
  for (const Case& c : cases) {
    Decimator d;
    int kept = 0;
    int64_t last = -1;
    bool spacingOk = true;
    for (int i = 0; i < 3000; ++i) {
      const int64_t t = 1794101400000LL + static_cast<int64_t>(i) * c.stepMs;
      if (!d.keep(t)) continue;
      if (last >= 0 && t - last != static_cast<int64_t>(c.stepMs) * c.keepEvery) spacingOk = false;
      if (i % c.keepEvery != 0) spacingOk = false;
      last = t;
      ++kept;
    }
    CHECKF(spacingOk, "%d ms epochs: kept rows not every %d", c.stepMs, c.keepEvery);
    CHECKF(kept == (3000 + c.keepEvery - 1) / c.keepEvery, "%d ms epochs: kept %d", c.stepMs, kept);
  }
  // 25 Hz with +-2 ms jitter: spacing stays 120 +- 4 ms
  Decimator d;
  int64_t last = -1;
  int64_t worst = 0;
  for (int i = 0; i < 3000; ++i) {
    const int64_t t = 1794101400000LL + i * 40 + static_cast<int64_t>(rnd() % 5) - 2;
    if (!d.keep(t)) continue;
    if (last >= 0) worst = std::max<int64_t>(worst, std::llabs(t - last - 120));
    last = t;
  }
  CHECKF(worst <= 4, "25 Hz with jitter: spacing off by %lld ms", static_cast<long long>(worst));
  tmini::info("25 Hz -> every 3rd epoch (120 ms, 8.3 Hz); 20 Hz -> every 2nd; 10 Hz -> all");
}

TEST(reportdata_decimator_duplicates_and_clock_jumps) {
  Decimator d;
  CHECK(d.keep(10000));
  CHECK(!d.keep(10000));  // duplicate
  CHECK(!d.keep(10099));
  CHECK(d.keep(10100));
  CHECK(!d.keep(9500));   // back by 600 ms: dropped
  CHECK(!d.keep(10150));
  CHECK(d.keep(8000));    // back by more than 1 s: starts again there
  CHECK(!d.keep(8050));
  CHECK(d.keep(8100));
  d.reset();
  CHECK(d.keep(5));
}

TEST(reportdata_columns_match_arch) {
  std::string h;
  for (int i = 0; i < lt::report::kNumMainColumns; ++i) {
    if (i) h += ",";
    h += lt::report::mainColumn(i);
  }
  CHECKF(h == kArchHeader, "main columns: %s", h.c_str());
  std::string r;
  for (int i = 0; i < lt::report::kNumColumns; ++i) {
    if (i) r += ",";
    r += lt::report::column(i);
    bool inMain = false;
    for (int m = 0; m < lt::report::kNumMainColumns; ++m) {
      inMain = inMain || std::strcmp(lt::report::column(i), lt::report::mainColumn(m)) == 0;
    }
    CHECKF(inMain, "report column %s is not a main log column", lt::report::column(i));
  }
  CHECKF(r == kReportHeader, "report columns: %s", r.c_str());
}

TEST(reportdata_filter_exact_row) {
  const std::string in = std::string(kArchHeader) +
                         "\n1794101400000,35.3949090,140.0880580,95.3,123.45,31.1,0.42,17,3,0.312,-1.045,"
                         "0.012,21.5,5519,87.5,122.25,91.5,62.0,1.12,4,3,2,45123,-0.123\n";
  FilterStats st;
  const std::string out = runFilter(in, 512, 4096, 100000, &st);
  const std::string expect = std::string(kReportHeader) +
                             "\n1794101400000,35.3949090,140.0880580,123.45,0.312,-1.045,5519,87.5,1.12,91.5,"
                             "62.0,3,2,45123\n";
  CHECKF(out == expect, "got:\n%s", out.c_str());
  CHECK(st.header && st.rowsIn == 1 && st.rowsOut == 1 && st.rowsBad == 0 && st.missingColumns == 0);
  CHECK(st.firstUtcMs == 1794101400000LL && st.lastUtcMs == 1794101400000LL);
}

TEST(reportdata_filter_any_chunking) {
  const std::string in = mainLog(3000, 40);
  FilterStats ref;
  const std::string expect = runFilter(in, 512, 1 << 20, 1 << 22, &ref);
  CHECKF(ref.rowsIn == 3000 && ref.rowsOut == 1000 && ref.rowsBad == 0, "in %lld out %lld bad %lld",
         static_cast<long long>(ref.rowsIn), static_cast<long long>(ref.rowsOut),
         static_cast<long long>(ref.rowsBad));
  CHECK(countLines(expect) == 1001);
  // Output buffer exactly maxOutput(): every row must still get through
  const int tight = 512 + lt::report::kNumColumns + 1;
  const int chunks[] = {1, 7, 64, 333, 4096, 0 /* random 1..900 */};
  for (int c : chunks) {
    FilterStats st;
    const std::string got = runFilter(in, 512, tight, c, &st);
    CHECKF(got == expect, "chunk %d: output differs (%zu vs %zu bytes)", c, got.size(), expect.size());
    CHECK(st.rowsIn == ref.rowsIn && st.rowsOut == ref.rowsOut && st.rowsBad == ref.rowsBad);
  }
}

TEST(reportdata_filter_damaged_input) {
  std::string in = std::string(kArchHeader) + "\r\n";
  in += "\n";                                         // empty line: ignored
  in += mainLine(1000, 0).insert(0, "") ;             // ok (t = 1000)
  in += "abc,1,2\n";                                  // no valid utc_ms
  in += std::string(600, '9') + "\n";                 // longer than the line buffer
  std::string withCr = mainLine(1100, 1);
  withCr.insert(withCr.size() - 1, "\r");             // CR LF
  in += withCr;
  std::string lt = mainLine(1200, 2);
  lt.replace(lt.find(",4,"), 3, ",<,");               // gear '<': written empty
  in += lt;
  in += mainLine(1300, 3).substr(0, 40);              // cut: no LF at the end of the file
  FilterStats st;
  const std::string out = runFilter(in, 512, 4096, 13, &st);
  CHECKF(st.rowsIn == 6, "rows in %lld", static_cast<long long>(st.rowsIn));
  CHECKF(st.rowsOut == 3, "rows out %lld", static_cast<long long>(st.rowsOut));
  CHECKF(st.rowsBad == 3, "rows bad %lld", static_cast<long long>(st.rowsBad));
  CHECK(st.badFields == 0);  // gear is not a report column: '<' never reaches the report
  CHECK(out.find('<') == std::string::npos && out.find('\r') == std::string::npos);
  CHECK(countLines(out) == 4);
  // A report column with '<' is written empty
  std::string in2 = std::string(kArchHeader) + "\n" + mainLine(1000, 0);
  in2.replace(in2.find(",17,3,"), 6, ",17,3,<");  // lon_g starts with '<'
  FilterStats st2;
  const std::string out2 = runFilter(in2, 512, 4096, 100000, &st2);
  CHECK(st2.badFields == 1 && st2.rowsOut == 1 && out2.find('<') == std::string::npos);
}

TEST(reportdata_filter_reordered_header_and_missing_columns) {
  const std::string in = "lap,rpm,utc_ms,lat,lon,extra\n2,5500,1000,35.1,140.2,x\n2,5600,1040,35.2,140.3,y\n"
                         "3,5700,1100,35.3,140.4,z\n";
  FilterStats st;
  const std::string out = runFilter(in, 512, 4096, 5, &st);
  const std::string expect = std::string(kReportHeader) + "\n1000,35.1,140.2,,,,5500,,,,,2,,\n1100,35.3,140.4,,,,5700,,,,,3,,\n";
  CHECKF(out == expect, "got:\n%s", out.c_str());
  CHECK(st.header && st.missingColumns == 9 && st.rowsIn == 3 && st.rowsOut == 2);
}

TEST(reportdata_filter_without_header_assumes_arch_order) {
  const std::string in = mainLine(5000, 0) + mainLine(5100, 1);
  FilterStats st;
  const std::string out = runFilter(in, 512, 4096, 100000, &st);
  CHECK(!st.header && st.rowsOut == 2 && st.rowsBad == 0);
  CHECK(out.compare(0, std::strlen(kReportHeader), kReportHeader) == 0);
  CHECK(out.find("\n5000,") != std::string::npos && out.find("\n5100,") != std::string::npos);
}

TEST(reportdata_escape_for_script) {
  const char in[] = "{\"name\":\"</script><b>\"}";
  char out[64];
  int used = 0;
  const int n = lt::report::escapeForScript(in, static_cast<int>(std::strlen(in)), out, sizeof out, &used);
  CHECK(std::string(out, static_cast<size_t>(n)) == "{\"name\":\"\\u003c/script>\\u003cb>\"}");
  CHECK(used == static_cast<int>(std::strlen(in)));
  // Room for 12 bytes: stops before the escape that does not fit
  const int k = lt::report::escapeForScript(in, static_cast<int>(std::strlen(in)), out, 12, &used);
  CHECK(k == 9 && used == 9);
}

TEST(reportdata_info_json) {
  FilterStats st;
  st.rowsIn = 45000;
  st.rowsOut = 15000;
  st.rowsBad = 1;
  st.firstUtcMs = 1794101400000LL;
  st.lastUtcMs = 1794103199880LL;
  st.header = true;
  const char* notes[] = {"ブーストは換算未確認 \"五分\"", "a<b"};
  lt::report::Info info;
  info.generator = "595_laptimer 0.1.0";
  info.log = &st;
  info.notes = notes;
  info.numNotes = 2;
  char out[512];
  const int n = lt::report::formatInfo(out, sizeof out, info);
  const std::string expect =
      "{\"generator\":\"595_laptimer 0.1.0\",\"period_ms\":100,\"rows_in\":45000,\"rows_out\":15000,"
      "\"rows_bad\":1,\"first_utc_ms\":1794101400000,\"last_utc_ms\":1794103199880,\"header\":true,"
      "\"missing_columns\":0,\"bad_fields\":0,\"notes\":[\"ブーストは換算未確認 \\\"五分\\\"\",\"a\\u003cb\"]}";
  CHECKF(n == static_cast<int>(expect.size()) && expect == out, "got %d: %s", n, out);
  CHECK(lt::report::formatInfo(out, 40, info) == -1);
}

TEST(reportdata_30min_session_size) {
  // 30 minutes at 25 Hz with every 5.2 column filled
  const int rows = 30 * 60 * 25;
  const std::string in = mainLog(rows, 40);
  FilterStats st;
  const std::string out = runFilter(in, 512, 8192, 4096, &st);
  const double inMb = static_cast<double>(in.size()) / 1048576.0;
  const double outMb = static_cast<double>(out.size()) / 1048576.0;
  tmini::info("30 min at 25 Hz: main log %.1f MB, %lld rows -> report data %.2f MB, %lld rows", inMb,
              static_cast<long long>(st.rowsIn), outMb, static_cast<long long>(st.rowsOut));
  CHECK(st.rowsOut == rows / 3);
  CHECKF(out.size() < 2u * 1048576u, "report data %.2f MB (budget 2 MB of the 3 MB file)", outMb);
}

TEST(webfmt_names_kinds_types) {
  using lt::webfmt::FileKind;
  using lt::webfmt::kindOf;
  CHECK(lt::webfmt::isSessionId("20261108_103000"));
  CHECK(!lt::webfmt::isSessionId("20261108-103000") && !lt::webfmt::isSessionId("2026110_1030001") &&
        !lt::webfmt::isSessionId("../../config") && !lt::webfmt::isSessionId(""));
  CHECK(lt::webfmt::isFileName("20261108_103000_sodegaura.vbo"));
  CHECK(!lt::webfmt::isFileName(".dl_x") && !lt::webfmt::isFileName("a/b") && !lt::webfmt::isFileName("..") &&
        !lt::webfmt::isFileName("") && !lt::webfmt::isFileName("a b") &&
        !lt::webfmt::isFileName("0123456789012345678901234567890123456789012345678901234567890123"));
  CHECK(lt::webfmt::isConfigName("courses.json") && lt::webfmt::isConfigName("wifi.json") &&
        !lt::webfmt::isConfigName("settings.json") && !lt::webfmt::isConfigName("../wifi.json"));
  CHECK(kindOf("20261108_103000_sodegaura.csv") == FileKind::MainCsv);
  CHECK(kindOf("20261108_103000_sodegaura_laps.csv") == FileKind::Laps);
  CHECK(kindOf("20261108_103000_sodegaura_imu.csv") == FileKind::Imu);
  CHECK(kindOf("20261108_103000_street_can.csv") == FileKind::Can);
  CHECK(kindOf("20261108_103000_sodegaura.vbo") == FileKind::Vbo);
  CHECK(kindOf("20261108_103000_sodegaura_raw.ubx") == FileKind::Ubx);
  CHECK(kindOf("meta.json") == FileKind::Meta);
  CHECK(kindOf(".dl_report.html") == FileKind::Marker && kindOf(".upload.tmp") == FileKind::Hidden);
  CHECK(lt::webfmt::countsAsDownload(FileKind::MainCsv) && lt::webfmt::countsAsDownload(FileKind::Vbo) &&
        !lt::webfmt::countsAsDownload(FileKind::Imu) && !lt::webfmt::countsAsDownload(FileKind::Ubx));
  CHECK(std::strcmp(lt::webfmt::contentType("a.vbo"), "application/octet-stream") == 0);
  CHECK(std::strcmp(lt::webfmt::contentType("a.csv"), "text/csv; charset=utf-8") == 0);
}

TEST(webfmt_dates) {
  struct Case {
    int64_t t;
    const char* http;
    const char* iso;
  } cases[] = {
      {0, "Thu, 01 Jan 1970 00:00:00 GMT", "1970-01-01T00:00:00Z"},
      {1794101400, "Sun, 08 Nov 2026 01:30:00 GMT", "2026-11-08T01:30:00Z"},
      {1835352000, "Mon, 28 Feb 2028 12:00:00 GMT", "2028-02-28T12:00:00Z"},
      {951782400, "Tue, 29 Feb 2000 00:00:00 GMT", "2000-02-29T00:00:00Z"},
      {4102444799, "Thu, 31 Dec 2099 23:59:59 GMT", "2099-12-31T23:59:59Z"},
  };
  for (const Case& c : cases) {
    char b[64];
    lt::webfmt::httpDate(b, sizeof b, c.t);
    CHECKF(std::strcmp(b, c.http) == 0, "%lld: %s", static_cast<long long>(c.t), b);
    lt::webfmt::isoDate(b, sizeof b, c.t);
    CHECKF(std::strcmp(b, c.iso) == 0, "%lld: %s", static_cast<long long>(c.t), b);
  }
  char small[10];
  CHECK(lt::webfmt::httpDate(small, sizeof small, 0) == -1);
}

TEST(webfmt_json_xml_dav) {
  char b[1024];
  int len = 0;
  lt::webfmt::putJsonString(b, sizeof b, &len, "袖ヶ浦 \"A\"\\\n<\x01");
  CHECKF(std::string(b) == "\"袖ヶ浦 \\\"A\\\"\\\\\\n\\u003c\\u0001\"", "json: %s", b);
  len = 0;
  lt::webfmt::putXml(b, sizeof b, &len, "a&b<c>\"");
  CHECK(std::string(b) == "a&amp;b&lt;c&gt;&quot;");
  len = 0;
  lt::webfmt::putInt(b, sizeof b, &len, -9223372036854775807LL - 1);
  CHECK(std::string(b) == "-9223372036854775808");
  len = 0;
  lt::webfmt::put(b, 4, &len, "abcd");
  CHECK(len == -1);

  lt::webfmt::DavEntry dir;
  dir.href = "/sessions/";
  dir.name = "sessions";
  dir.collection = true;
  dir.mtimeS = 1794101400;
  dir.quotaFree = 1700000000;
  dir.quotaUsed = 120000000;
  int n = lt::webfmt::davResponse(b, sizeof b, dir);
  std::string expect =
      "<D:response><D:href>/sessions/</D:href><D:propstat><D:prop><D:displayname>sessions</D:displayname>"
      "<D:resourcetype><D:collection/></D:resourcetype><D:quota-available-bytes>1700000000"
      "</D:quota-available-bytes><D:quota-used-bytes>120000000</D:quota-used-bytes>"
      "<D:getlastmodified>Sun, 08 Nov 2026 01:30:00 GMT</D:getlastmodified>"
      "<D:creationdate>2026-11-08T01:30:00Z</D:creationdate></D:prop><D:status>HTTP/1.1 200 OK</D:status>"
      "</D:propstat></D:response>\n";
  CHECKF(n == static_cast<int>(expect.size()) && expect == b, "dir: %s", b);

  lt::webfmt::DavEntry file;
  file.href = "/sessions/20261108_103000/a b.vbo";
  file.name = "a b.vbo";
  file.size = 4824388;
  n = lt::webfmt::davResponse(b, sizeof b, file);
  expect =
      "<D:response><D:href>/sessions/20261108_103000/a%20b.vbo</D:href><D:propstat><D:prop>"
      "<D:displayname>a b.vbo</D:displayname><D:resourcetype/><D:getcontentlength>4824388"
      "</D:getcontentlength><D:getcontenttype>application/octet-stream</D:getcontenttype></D:prop>"
      "<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>\n";
  CHECKF(n == static_cast<int>(expect.size()) && expect == b, "file: %s", b);
  CHECK(lt::webfmt::davResponse(b, 100, file) == -1);
}
