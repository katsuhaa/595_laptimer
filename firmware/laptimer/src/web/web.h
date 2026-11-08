// web.h - the laptimer's Web server for iPhone Safari and the Mac Finder (プロジェクト計画 §4.7,
// UI設計 §9, 調査 §3-§4). Wi-Fi joining and mDNS are done by the app; this module only serves.
//
// iPhone (Safari, http://laptimer.local/ or the IP):
//   GET  /  /maint                 index.html: status, sessions, downloads, delete, config uploads,
//                                  maintenance editor, course from a RaceChrono VBO (gzip in flash)
//   GET  /report?id=<id>           web/report.html with the session's data put in (streamed; the main
//                                  log decimated to ~10 Hz by lt::report::LogFilter while it is read)
//   GET  /report?id=<id>&save=1    the same as the attachment report_<id>.html; marks .dl_report.html
//   GET  /report.html              the report page alone (it fetches /api/reportdata?id=)
//   GET  /api/reportdata?id=<id>   the report's data blocks alone
//   GET  /dl?id=<id>&f=<file>      a session file as an attachment; marks .dl_<file> when completely sent
//   GET  /api/status, /api/sessions, /api/session?id=, /api/config (names, sizes, dates)
//   GET  /config/<name>[?save=1]   a config file (wifi.json is refused: it holds passwords)
//   POST /api/delete?id=<id>       deletes the session folder (refused for the session being recorded)
//   POST /upload?name=<config>     courses.json / maint.json / checklist.json / wifi.json: stored to a
//                                  temp file, checked (lt::json::check; courses and maint also with
//                                  lt::CourseDb::check / lt::Maint::check), then it replaces
//                                  /config/<name> (the old one kept as /config/.prev_<name>) and
//                                  hooks.configChanged(name) is called; refused with line/column/reason
//   POST /api/maintdone?id=<item>  hooks.maintDone(id)
//   POST /api/odo?km=<value>       hooks.setOdometer(km): the ODO base value (0..999999.9 km)
//   POST /api/format               hooks.formatStorage(), only while hooks.storageMounted() is false
//   POST /api/course_from_vbo      the head of a RaceChrono VBO export (up to [data], sent by the page)
//                                  stored as kCourseVboTmp, hooks.courseFromVbo() makes the course,
//                                  then hooks.configChanged("courses.json")
// Mac (Finder "サーバへ接続", http://laptimer.local): read-only WebDAV class 1 - OPTIONS, PROPFIND
//   (Depth 0/1) of /, /sessions/, /best/, /config/, /replay/ and their files, GET/HEAD of files.
//   Write methods get 403 (PUT, DELETE, MKCOL, MOVE, COPY, PROPPATCH, POST) or 405 (LOCK, UNLOCK).
//   Hidden: names starting with '.', the markers, wifi.json. A WebDAV GET does not mark downloads
//   (the Finder also reads files for previews).
//
// XTSD access: every file operation is inside hooks.storageLock()/storageUnlock(), one short
// section per 8 KB piece while streaming (the logger shares the SPI bus). Hooks are called from the
// task that calls web::handle(), never while the storage lock is held.
#pragma once

#include <FS.h>
#include <stdint.h>

namespace web {

constexpr const char* kCourseVboTmp = "/config/.course_vbo.tmp";

// What the pages show about the device. web fills ip / ssid / rssi from WiFi and sets the rest to
// "unknown" before calling hooks.status(), which fills what the app knows.
struct Status {
  char fw[16];           // firmware version, e.g. "0.1.0"
  char hostname[32];     // mDNS name without ".local", e.g. "laptimer"
  char ip[16];           // "172.20.10.3"
  char ssid[33];         // joined network
  int rssi;              // dBm
  uint64_t freeBytes;    // XTSD free / total
  uint64_t totalBytes;
  float remainHours;     // recording time left at the full data rate (lt::StorePlan); < 0 unknown
  bool storageLow;       // the 20 % warning
  int64_t utcMs;         // device clock (UTC); 0 = not set
  int tzMin;             // local time offset in minutes (540 = JST)
  float odoKm;           // for the maintenance page; < 0 unknown
  float trackHours;      // < 0 unknown
};

// Callbacks into the app. Every pointer may be null (that feature then answers "not available").
struct Hooks {
  void (*status)(Status* s);
  // The XTSD is shared with the logger: true = locked (within timeoutMs), false = busy.
  bool (*storageLock)(uint32_t timeoutMs);
  void (*storageUnlock)();
  // /config/<name> was replaced by an upload (or by courseFromVbo): reload it.
  void (*configChanged)(const char* name);
  // "実施済み" from the maintenance page: mark the item done now and rewrite maint.json.
  bool (*maintDone)(const char* id);
  // The session being recorded ("" or null when none): never deleted, marked "記録中".
  const char* (*activeSessionId)();
  // tmpPath (kCourseVboTmp on the same fs) holds the head of a RaceChrono VBO export: make the
  // course (lt::CourseDb::parseRaceChronoVbo + put) and write /config/courses.json. msg: a short
  // UTF-8 text for the page (the course name on success, the reason on failure).
  bool (*courseFromVbo)(const char* tmpPath, char* msg, int msgCap);
  // A session folder was deleted, or a download marker (.dl_<file>) was written: the device's
  // session list and its undownloaded count are out of date.
  void (*sessionsChanged)();
  // Optional (null = feature off). false: the XTSD has no usable FAT; /api/status reports
  // "storage_mounted":false and only then POST /api/format calls formatStorage().
  bool (*storageMounted)();
  bool (*formatStorage)();
  // Optional: the owner sets the car's ODO (UI設計 §9: base value + integrated speed). false = not taken.
  bool (*setOdometer)(float km);
};

// Starts the server on port 80 (call once the network stack is up, e.g. after the first join;
// it keeps listening across Wi-Fi reconnects). fs = the XTSD (hw::sd()). false: out of memory.
bool begin(fs::FS& fs, const Hooks& hooks);
// Serves what is waiting; returns at once when nothing is. A download runs inside this call.
void handle();
// Stops the server and frees its memory.
void end();
bool running();
// The last download / upload in one line, e.g. "dl 20261108_103000_sodegaura.vbo 4.82 MB 3.1 s 1.55 MB/s"
// (for the device screen: XTSD -> Wi-Fi speed, 実装計画 1-8). "-" before the first one.
const char* lastTransfer();

}  // namespace web
