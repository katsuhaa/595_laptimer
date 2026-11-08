// core/reportdata.h - the session report built from the 25 Hz main log, and small formatting
// helpers of the web module. Shared by the device (web module, streaming from the XTSD) and the
// host `report` tool, so both produce the same file. No heap: the caller passes every buffer.
//
// The report is web/report.html with the session's data put where kMarker is:
//   kMetaOpen  meta.json                                      ('<' escaped as <)
//   kLapsOpen  <id>_<cid>_laps.csv as stored                   ('<' escaped, never present)
//   kLogOpen   main log decimated to ~10 Hz, kColumns only     (fields checked: [0-9A-Za-z.+-])
//   kInfoOpen  what was done: rows in / out / dropped, period, generator, notes
// each block followed by kClose. The page reads the blocks by id; the HTML parser never ends a
// block early because the data never contains '<'.
#pragma once

#include <cstdint>

namespace lt {
namespace report {

constexpr char kMarker[] = "<!--LT_DATA-->";
constexpr char kMetaOpen[] = "<script type=\"application/json\" id=\"lt-meta\">";
constexpr char kLapsOpen[] = "<script type=\"text/csv\" id=\"lt-laps\">";
constexpr char kLogOpen[] = "<script type=\"text/csv\" id=\"lt-log\">";
constexpr char kInfoOpen[] = "<script type=\"application/json\" id=\"lt-info\">";
constexpr char kClose[] = "</script>\n";

constexpr int64_t kPeriodMs = 100;     // report rows: one every 100 ms or more (about 10 Hz)
constexpr int64_t kBackJumpMs = 1000;  // the clock jumping back more than this restarts the decimator

// The main log header of ARCH 5.2, in order.
constexpr int kNumMainColumns = 24;
const char* mainColumn(int i);
// The main log columns copied into the report, in this order.
constexpr int kNumColumns = 14;
const char* column(int i);

// Keeps the first row, then a row whose time is at least periodMs after the last kept row.
// With 25 Hz epochs (40 ms) that is every third epoch (120 ms); with 10 or 20 Hz every 100 ms.
// Rows earlier than the last kept one are dropped, unless the clock jumped back by more than
// kBackJumpMs (then the decimator starts again from that row).
class Decimator {
 public:
  explicit Decimator(int64_t periodMs = kPeriodMs) : periodMs_(periodMs) {}
  void reset() { have_ = false; }
  bool keep(int64_t utcMs);

 private:
  int64_t periodMs_;
  int64_t last_ = 0;
  bool have_ = false;
};

struct FilterStats {
  int64_t rowsIn = 0;    // data lines read (after the header)
  int64_t rowsOut = 0;   // rows written to the report
  int64_t rowsBad = 0;   // dropped: no valid utc_ms, longer than the line buffer, or no LF at the end
  int64_t firstUtcMs = 0;  // of the rows written
  int64_t lastUtcMs = 0;
  bool header = false;     // the first line was a header with utc_ms
  int missingColumns = 0;  // report columns not in the header (written empty)
  int badFields = 0;       // fields with other characters than [0-9A-Za-z.+-] (written empty)
};

// Main log CSV bytes in, report CSV bytes out: the header row (kColumns), then the kept rows.
// Input may be cut anywhere (file reads of any size). LF line ends; a CR before LF is ignored.
class LogFilter {
 public:
  // lineBuf holds one input line; 512 bytes is plenty (a 5.2 line is under 200 characters).
  void begin(char* lineBuf, int lineCap, int64_t periodMs = kPeriodMs);
  // Consumes input and appends output to out[0..cap). Returns the input bytes consumed: fewer
  // than n when out is full; then send out and call again with at least maxOutput() free bytes.
  int feed(const char* in, int n, char* out, int cap, int* produced);
  // End of the file: a last line without LF is incomplete (power cut) and is dropped.
  void finish();
  int maxOutput() const { return lineCap_ + kNumColumns + 1; }
  const FilterStats& stats() const { return st_; }

 private:
  bool line(char* out, int cap, int* produced);  // false: no room in out
  bool header(int len);
  int pick(int len, char* out);

  static constexpr int kMaxFields = 48;
  char* buf_ = nullptr;
  int lineCap_ = 0;
  int len_ = 0;
  bool overflow_ = false;
  bool seenFirst_ = false;
  int map_[kNumColumns] = {};
  int utcCol_ = -1;
  int numFields_ = 0;
  int fs_[kMaxFields] = {};
  int fe_[kMaxFields] = {};
  Decimator dec_;
  FilterStats st_;
};

// Copies text for a script block, writing '<' as < (valid inside JSON strings, and the
// only character that could end the block). Returns bytes written; *consumed = input used.
int escapeForScript(const char* in, int n, char* out, int cap, int* consumed);

struct Info {
  const char* generator = "";  // "595_laptimer 0.1.0" (device) or "go.sh report" (host)
  int64_t periodMs = kPeriodMs;
  const FilterStats* log = nullptr;
  const char* const* notes = nullptr;  // UTF-8 lines shown under the report (may be Japanese)
  int numNotes = 0;
};
// The kInfoOpen block's JSON. Returns its length, or -1 when cap is too small.
int formatInfo(char* out, int cap, const Info& info);

}  // namespace report

// Helpers of the web module that need no Arduino (tested on the host).
namespace webfmt {

// Appends to out[*len..cap); on overflow *len becomes -1 and stays there.
void put(char* out, int cap, int* len, const char* s);
void putN(char* out, int cap, int* len, const char* s, int n);
void putInt(char* out, int cap, int* len, int64_t v);
// A JSON string with quotes; '<' and control characters escaped, UTF-8 passed through.
void putJsonString(char* out, int cap, int* len, const char* s);
// XML text: & < > " escaped.
void putXml(char* out, int cap, int* len, const char* s);

// Session folder name: YYYYMMDD_HHMMSS.
bool isSessionId(const char* s);
// A file name the web module serves: 1..63 characters of [A-Za-z0-9._-], not starting with '.'.
bool isFileName(const char* s);
// Config files the iPhone may upload.
bool isConfigName(const char* s);

enum class FileKind { MainCsv, Vbo, Ubx, Imu, Can, Laps, Meta, Marker, Hidden, Other };
FileKind kindOf(const char* name);
// A complete download of one of these marks the session as downloaded (ARCH 5.1), as does the
// report (marker ".dl_report.html").
bool countsAsDownload(FileKind k);
constexpr char kMarkerPrefix[] = ".dl_";
constexpr char kReportMarkerFile[] = "report.html";  // marker name: ".dl_report.html"

// Content-Type for a download: CSV as text, VBO/UBX/bin as octet-stream (bytes kept exactly).
const char* contentType(const char* name);

// "Sun, 08 Nov 2026 01:30:00 GMT" (HTTP date) and "2026-11-08T01:30:00Z" from Unix seconds.
int httpDate(char* out, int cap, int64_t epochS);
int isoDate(char* out, int cap, int64_t epochS);

// One <D:response> of a PROPFIND multistatus (read-only WebDAV for the Mac Finder).
struct DavEntry {
  const char* href = "/";     // absolute path, '/' at the end for a collection
  const char* name = "";      // displayname
  bool collection = false;
  int64_t size = 0;           // files
  int64_t mtimeS = 0;         // <= 0: not written
  int64_t quotaFree = -1;     // collections: written when >= 0
  int64_t quotaUsed = -1;
};
constexpr char kDavHead[] = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<D:multistatus xmlns:D=\"DAV:\">\n";
constexpr char kDavTail[] = "</D:multistatus>\n";
int davResponse(char* out, int cap, const DavEntry& e);  // length, -1 when cap is too small

}  // namespace webfmt
}  // namespace lt
