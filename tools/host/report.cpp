// tools/host/report.cpp - the session report as one HTML file, made on the Mac (go.sh report).
//   report <input>... [--course <RaceChrono .vbo>] [--courses <courses.json>] [--html <report.html>] [--out <dir>]
//       input: a RaceChrono VBO export (with [laptiming]), an unzipped RaceChrono .rcz directory (its
//       lines come from --course), or a session folder in the ARCH 5 layout (sessions/<id>/).
//       Writes <out>/report_<id>.html, the same file the device sends for "レポートをダウンロード"
//       (web/report.html + the session's data, built like the device: tools/host/report_build), and
//       checks it: lap count and lap times equal to the lap timer, the decimated log, the file size
//       (30-minute session <= 3 MB). Last line: RESULT: PASS or FAIL.
//   report --session <input> <card root> [--course ..] [--courses ..]
//       Writes the session folder <card root>/sessions/<id>/ (for the Web page tests' mock server).
//   report --data <session folder>
//       The data blocks on stdout (the mock server puts them between the parts of report.html).
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "core/reportdata.h"
#include "tools/host/rcz.h"
#include "tools/host/report_build.h"
#include "tools/host/report_session.h"
#include "tools/host/vboread.h"

namespace {

constexpr double kLimitS = 0.05;              // RaceChrono comparison (.rcz), as go.sh laps
constexpr double kMaxBytesPer30Min = 3.0e6;   // 実装計画 0-9: 30-minute session <= 3 MB
const char kGenerator[] = "go.sh report";

bool isDir(const std::string& p) {
  struct stat st{};
  return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool isFile(const std::string& p) {
  struct stat st{};
  return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
bool endsWith(const std::string& s, const char* suf) {
  const size_t k = std::strlen(suf);
  return s.size() >= k && s.compare(s.size() - k, k, suf) == 0;
}

std::string baseName(std::string p) {
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  const size_t s = p.find_last_of('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

struct Args {
  std::vector<std::string> inputs;
  std::string coursePath, coursesJson, html = "web/report.html", out = "out", sessionRoot;
  bool session = false, data = false;
};

int fails = 0;
void check(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++fails;
}

bool loadSession(const std::string& in, const Args& a, const vboread::File* course, rs::Session* s, std::string* err) {
  rs::Options opt;
  opt.course = course;
  opt.coursesJson = a.coursesJson;
  if (isDir(in)) return rs::fromRcz(in, opt, s, err);
  return rs::fromVboExport(in, opt, s, err);
}

// "1,1000,2000,1000,250;750" lines of the laps CSV (a header row is skipped)
std::vector<rs::LapRow> parseLaps(const std::string& text) {
  std::vector<rs::LapRow> v;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line.compare(0, 3, "lap") == 0) continue;
    rs::LapRow l;
    std::istringstream f(line);
    std::string a, b, c, d, e;
    std::getline(f, a, ',');
    std::getline(f, b, ',');
    std::getline(f, c, ',');
    std::getline(f, d, ',');
    std::getline(f, e);
    l.lap = std::atoi(a.c_str());
    l.startUtcMs = std::atoll(b.c_str());
    l.endUtcMs = std::atoll(c.c_str());
    l.lapMs = std::atoll(d.c_str());
    std::istringstream s(e);
    std::string x;
    while (std::getline(s, x, ';')) {
      if (!x.empty() && x != "\r") l.sectorsMs.push_back(std::atoll(x.c_str()));
    }
    v.push_back(l);
  }
  return v;
}

// Checks of a written report file; `expect` = the laps it must show
void checkReport(const std::string& path, const std::vector<rs::LapRow>& expect, const std::string& expectWhat) {
  std::string html;
  {
    FILE* fp = std::fopen(path.c_str(), "rb");
    char buf[65536];
    size_t n = 0;
    while (fp && (n = std::fread(buf, 1, sizeof buf, fp)) > 0) html.append(buf, n);
    if (fp) std::fclose(fp);
  }
  std::string meta, laps, log, info;
  const bool blocks = rb::blockText(html, lt::report::kMetaOpen, &meta) &&
                      rb::blockText(html, lt::report::kLapsOpen, &laps) &&
                      rb::blockText(html, lt::report::kLogOpen, &log) &&
                      rb::blockText(html, lt::report::kInfoOpen, &info);
  check(blocks && html.find(lt::report::kMarker) == std::string::npos && endsWith(html, "</html>\n"),
        "html: one block each of meta, laps, log, info; the page code after them");
  if (!blocks) return;

  const std::vector<rs::LapRow> got = parseLaps(laps);
  bool same = got.size() == expect.size();
  int64_t worst = 0;
  for (size_t i = 0; same && i < got.size(); ++i) {
    same = got[i].lap == expect[i].lap && got[i].sectorsMs == expect[i].sectorsMs;
    worst = std::max<int64_t>(worst, std::llabs(got[i].lapMs - expect[i].lapMs));
  }
  same = same && worst == 0;
  char b[256];
  std::snprintf(b, sizeof b, "laps: %zu laps in the report, %zu from %s, lap times %s", got.size(), expect.size(),
                expectWhat.c_str(), same ? "equal (sectors too)" : "DIFFER");
  check(same, b);

  // The log block: report columns, rows as the info block says, time increasing, rows in every lap
  std::istringstream in(log);
  std::string line, header;
  std::getline(in, header);
  std::string want;
  for (int i = 0; i < lt::report::kNumColumns; ++i) want += (i ? "," : "") + std::string(lt::report::column(i));
  long rows = 0;
  long long prev = -1;
  bool increasing = true;
  std::vector<long> perLap(expect.size() + 2, 0);
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    ++rows;
    const long long t = std::atoll(line.c_str());
    if (t <= prev) increasing = false;
    prev = t;
    // lap is the 12th column
    size_t p = 0;
    for (int k = 0; k < 11 && p != std::string::npos; ++k) p = line.find(',', p + 1);
    const int lap = p == std::string::npos ? 0 : std::atoi(line.c_str() + p + 1);
    if (lap > 0 && lap <= static_cast<int>(expect.size())) ++perLap[static_cast<size_t>(lap)];
  }
  const size_t ro = info.find("\"rows_out\":");
  const long infoRows = ro == std::string::npos ? -1 : std::atol(info.c_str() + ro + 11);
  bool lapsHaveRows = true;
  for (size_t k = 1; k <= expect.size(); ++k) lapsHaveRows = lapsHaveRows && perLap[k] > 0;
  std::snprintf(b, sizeof b, "log: %ld rows (info says %ld), report columns, time increasing, rows in every lap",
                rows, infoRows);
  check(header == want && rows == infoRows && increasing && lapsHaveRows, b);

  // Size against the 30-minute target
  long long first = std::atoll(info.c_str() + info.find("\"first_utc_ms\":") + 15);
  long long last = std::atoll(info.c_str() + info.find("\"last_utc_ms\":") + 14);
  const double minutes = static_cast<double>(last - first) / 60000.0;
  const double per30 = minutes > 1.0 ? static_cast<double>(html.size()) * 30.0 / minutes : static_cast<double>(html.size());
  std::snprintf(b, sizeof b, "size: %.2f MB for %.1f minutes = %.2f MB per 30 minutes (limit %.1f MB)",
                static_cast<double>(html.size()) / 1e6, minutes, per30 / 1e6, kMaxBytesPer30Min / 1e6);
  check(per30 <= kMaxBytesPer30Min, b);
}

bool writeFile(const std::string& path, const std::string& text) {
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) return false;
  const bool ok = std::fwrite(text.data(), 1, text.size(), fp) == text.size();
  std::fclose(fp);
  return ok;
}

// Report from a session folder, as the device builds it; returns the output path ("" on failure)
std::string buildReport(const std::string& dir, const Args& a, const std::string& id) {
  rb::Template t;
  std::string err;
  if (!rb::loadTemplate(a.html, &t, &err)) {
    check(false, "template: " + err);
    return "";
  }
  rb::Data d;
  if (!rb::dataBlocks(dir, kGenerator, {}, &d, &err)) {
    check(false, "data: " + err);
    return "";
  }
  ::mkdir(a.out.c_str(), 0755);
  const std::string path = a.out + "/report_" + id + ".html";
  if (!writeFile(path, t.head + d.blocks + t.tail)) {
    check(false, "cannot write " + path);
    return "";
  }
  std::printf("wrote %s (%.2f MB; page code %.0f KB; log %lld rows -> %lld rows)\n", path.c_str(),
              static_cast<double>(t.head.size() + d.blocks.size() + t.tail.size()) / 1e6,
              static_cast<double>(t.head.size() + t.tail.size()) / 1024.0, static_cast<long long>(d.log.rowsIn),
              static_cast<long long>(d.log.rowsOut));
  return path;
}

void runInput(const std::string& in, const Args& a, const vboread::File* course) {
  std::printf("\n== %s\n", baseName(in).c_str());
  if (isDir(in) && isFile(in + "/meta.json")) {  // a session folder
    std::string laps;
    std::vector<rs::LapRow> expect;
    const std::string id = baseName(in);
    rb::Data d;
    std::string err;
    if (!rb::dataBlocks(in, kGenerator, {}, &d, &err)) {
      check(false, err);
      return;
    }
    rb::blockText(d.blocks, lt::report::kLapsOpen, &laps);
    FILE* fp = std::fopen((in + "/" + d.lapsName).c_str(), "rb");
    std::string text;
    char buf[4096];
    size_t n = 0;
    while (fp && (n = std::fread(buf, 1, sizeof buf, fp)) > 0) text.append(buf, n);
    if (fp) std::fclose(fp);
    expect = parseLaps(text);
    const std::string path = buildReport(in, a, id);
    if (!path.empty()) checkReport(path, expect, "the session's laps file");
    return;
  }
  rs::Session s;
  std::string err;
  if (!loadSession(in, a, course, &s, &err)) {
    check(false, err);
    return;
  }
  std::printf("session %s: %s, GPS %.0f Hz, %zu fixes, %.1f minutes, course %s (%s)\n", s.id.c_str(),
              s.courseName.c_str(), s.rateHz, s.rows.size(),
              static_cast<double>(s.rows.back().utcMs - s.rows.front().utcMs) / 60000.0, s.courseId.c_str(),
              s.lineNames.empty() ? "" : std::to_string(s.lineNames.size()).append(" lines").c_str());
  for (const std::string& note : s.notes) std::printf("  note: %s\n", note.c_str());
  // The session goes through a folder, so the report is built exactly as on the device
  const std::string root = "build/host/report_sessions";
  std::string dir;
  if (!rs::writeFolder(s, root, &dir, &err)) {
    check(false, err);
    return;
  }
  // The lap timer run again on the same fixes: what the report must show
  std::vector<rs::LapRow> expect;
  for (const lt::LapResult& r : rs::lapTimer(s, s.course)) {
    rs::LapRow l;
    l.lap = r.lapNo;
    l.lapMs = (r.lapUs + 500) / 1000;
    for (int k = 0; k < r.numSplits; ++k) l.sectorsMs.push_back((r.splitUs[k] + 500) / 1000);
    expect.push_back(l);
  }
  std::printf("lap   time       sectors\n");
  for (const rs::LapRow& l : s.laps) {
    std::string sec;
    for (int64_t x : l.sectorsMs) sec += " " + (x < 60000 ? rs::lapText(x).substr(2) : rs::lapText(x));
    std::printf("%3d   %s%s\n", l.lap, rs::lapText(l.lapMs).c_str(), sec.c_str());
  }
  if (!s.raceChronoLaps.empty()) {
    int matched = 0;
    double worst = 0.0;
    for (const auto& rc : s.raceChronoLaps) {
      for (const rs::LapRow& l : s.laps) {
        if (std::llabs(l.startUtcMs - rc.first) <= 500 && std::llabs(l.endUtcMs - rc.second) <= 500) {
          ++matched;
          worst = std::max(worst, std::fabs(static_cast<double>(l.lapMs - (rc.second - rc.first)) * 1e-3));
        }
      }
    }
    char b[200];
    std::snprintf(b, sizeof b, "RaceChrono: %d/%zu laps match, max difference %.3f s (limit %.2f s)", matched,
                  s.raceChronoLaps.size(), worst, kLimitS);
    check(matched == static_cast<int>(s.raceChronoLaps.size()) &&
              s.laps.size() == s.raceChronoLaps.size() && worst <= kLimitS,
          b);
  }
  const std::string path = buildReport(dir, a, s.id);
  if (!path.empty()) checkReport(path, expect, "the lap timer");
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    auto next = [&](std::string* v) {
      if (i + 1 < argc) *v = argv[++i];
    };
    if (s == "--course") next(&a.coursePath);
    else if (s == "--courses") next(&a.coursesJson);
    else if (s == "--html") next(&a.html);
    else if (s == "--out") next(&a.out);
    else if (s == "--session") a.session = true;
    else if (s == "--data") a.data = true;
    else a.inputs.push_back(s);
  }
  vboread::File courseFile;
  const vboread::File* course = nullptr;
  if (!a.coursePath.empty()) {
    std::string err;
    if (!vboread::read(a.coursePath, &courseFile, &err)) {
      std::printf("FAIL %s\n", err.c_str());
      return 1;
    }
    course = &courseFile;
  }
  if (a.data) {  // mock server: data blocks of a session folder on stdout
    if (a.inputs.size() != 1) return 2;
    rb::Data d;
    std::string err;
    if (!rb::dataBlocks(a.inputs[0], "mock_server (go.sh report)", {}, &d, &err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 1;
    }
    std::fwrite(d.blocks.data(), 1, d.blocks.size(), stdout);
    return 0;
  }
  if (a.session) {  // session folder for the mock server
    if (a.inputs.size() != 2) {
      std::printf("usage: report --session <VBO export | unzipped .rcz> <card root> [--course <vbo>]\n");
      return 2;
    }
    rs::Session s;
    std::string err, dir;
    if (!loadSession(a.inputs[0], a, course, &s, &err) || !rs::writeFolder(s, a.inputs[1], &dir, &err)) {
      std::printf("FAIL %s\n", err.c_str());
      return 1;
    }
    std::printf("wrote %s (%zu rows, %zu laps)\n", dir.c_str(), s.rows.size(), s.laps.size());
    return 0;
  }
  if (a.inputs.empty()) {
    std::printf("usage: report <VBO export | unzipped .rcz | session folder>... [--course <vbo>] [--courses <json>]\n"
                "       report --session <input> <card root>   |   report --data <session folder>\n");
    return 2;
  }
  // A VBO export given as input is also the course of the .rcz inputs
  if (!course) {
    for (const std::string& in : a.inputs) {
      std::string err;
      if (!isDir(in) && endsWith(in, ".vbo") && vboread::read(in, &courseFile, &err) && !courseFile.gates.empty()) {
        course = &courseFile;
        break;
      }
    }
  }
  for (const std::string& in : a.inputs) runInput(in, a, course);
  std::printf("\nRESULT: %s\n", fails ? "FAIL" : "PASS");
  return fails ? 1 : 0;
}
