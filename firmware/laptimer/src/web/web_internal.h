// web_internal.h - shared by web.cpp, web_report.cpp and web_dav.cpp (not for the app).
#pragma once

#include <Arduino.h>
#include <FS.h>
#include <WebServer.h>

#include "web.h"

namespace web {
namespace detail {

constexpr uint32_t kLockMs = 3000;     // waiting for the logger to release the XTSD
constexpr size_t kChunk = 8192;        // one file piece per lock while streaming
constexpr size_t kOutCap = 4096;       // response buffer (one chunk of the chunked encoding)
constexpr int kLineCap = 512;          // one main-log line (lt::report::LogFilter)
constexpr int kFilterOutCap = 4096;    // report rows waiting to be sent
constexpr int kMaxSessions = 256;      // listed sessions
constexpr int kMaxFiles = 32;          // files in one session folder
constexpr int kMaxMeta = 2048;         // meta.json in the session list

struct FileInfo {
  char name[64];
  uint32_t size;
  uint8_t kind;  // lt::webfmt::FileKind
  bool dl;
};

struct Scratch {  // big buffers, allocated once in begin() (PSRAM when there is some)
  char ids[kMaxSessions][16];
  FileInfo files[kMaxFiles];
  char markers[kMaxFiles][64];
  char meta[kMaxMeta];
  char json[1024];
};

struct Ctx {
  WebServer* srv = nullptr;
  fs::FS* fs = nullptr;
  Hooks h{};
  uint8_t* buf = nullptr;   // kChunk
  uint8_t* out = nullptr;   // kOutCap
  char* line = nullptr;     // kLineCap
  char* fout = nullptr;     // kFilterOutCap
  Scratch* sc = nullptr;
};
extern Ctx g;

// The XTSD for one short section (hooks.storageLock / storageUnlock).
class Lock {
 public:
  explicit Lock(uint32_t ms = kLockMs);
  ~Lock();
  bool ok() const { return ok_; }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;

 private:
  bool ok_;
};

// A response written straight to the client, for bodies whose length is not known in advance:
// status line and headers, then the body in chunks (HTTP/1.1) or until the connection closes
// (HTTP/1.0). Every write is checked; after the first failure nothing more is sent.
class Out {
 public:
  bool begin(int code, const char* reason, const char* type, const char* extraHeaders = nullptr);
  bool write(const void* p, size_t n);
  bool print(const char* s);
  bool end();  // flush and the last chunk; true when everything went out
  bool ok() const { return ok_; }
  uint64_t sent() const { return sent_; }

 private:
  bool flush();
  bool raw(const void* p, size_t n);
  NetworkClient* c_ = nullptr;
  bool chunked_ = false;
  bool ok_ = false;
  size_t n_ = 0;
  uint64_t sent_ = 0;
};

// Small replies (fixed length, through WebServer::send)
void sendJson(int code, const char* json);
void sendError(int code, const char* error, const char* message);
void sendBusy();

const char* activeId();           // "" when none
bool storageMounted();            // true when there is no hook
void notifySessions();            // hooks.sessionsChanged
const char* lastTransferText();
void setLastTransfer(const char* text);
void statusFill(Status* s);       // WiFi values, then hooks.status
bool readSmall(const char* path, char* buf, int cap, int* len);  // whole file under one lock; false if missing / larger
bool touch(const char* path);     // empty marker file
// Streams an open file of known size with Content-Length (the webcheck download pattern).
// attachName: Content-Disposition file name, or null. Returns true when every byte was sent.
bool sendFile(File& f, size_t size, const char* type, const char* attachName, bool headOnly);

// web_report.cpp
void handleReport();       // GET /report
void handleReportData();   // GET /api/reportdata
// web_dav.cpp: the catch-all handler (uploads, read-only WebDAV, 404). Added last.
RequestHandler* makeRouter();
// web.cpp: the two upload routes (called by the router)
bool isUploadUri(const String& uri);
void uploadData(HTTPUpload& u);
void uploadDone();
void uploadReset();

}  // namespace detail
}  // namespace web
