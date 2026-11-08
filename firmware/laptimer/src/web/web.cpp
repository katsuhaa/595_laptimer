// web.cpp - routes, the JSON API, downloads and uploads (see web.h).
// The download and upload code follows firmware/webcheck (P0-W): setContentLength + send(200, type, "")
// + NetworkClient::write of each piece, Content-Disposition for attachments, and the HTTPUpload
// START/WRITE/END/ABORTED sequence into a temp file that is checked before it replaces anything.
#include "web.h"

#include <WiFi.h>
#include <esp_heap_caps.h>
#include <stdlib.h>

#include <new>

#include "lt_core.h"
#include "web_assets.h"
#include "web_internal.h"

namespace web {
namespace detail {

Ctx g;

namespace {

constexpr const char* kUploadTmp = "/config/.upload.tmp";
constexpr size_t kMaxConfig = 64 * 1024;
constexpr size_t kMaxVboHead = 256 * 1024;  // the page sends only the part up to [data] (a few KB)
char lastText[112] = "-";

using lt::webfmt::put;
using lt::webfmt::putInt;
using lt::webfmt::putJsonString;

void* alloc(size_t n, bool psram) {
  void* p = nullptr;
  if (psram) p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return p;
}

void putFloat(char* b, int cap, int* len, float v, int decimals) {
  char t[24];
  if (!(v == v) || v < -1e15f || v > 1e15f) {
    put(b, cap, len, "null");
    return;
  }
  snprintf(t, sizeof t, "%.*f", decimals, static_cast<double>(v));
  put(b, cap, len, t);
}

const char* kindName(lt::webfmt::FileKind k) {
  using lt::webfmt::FileKind;
  switch (k) {
    case FileKind::MainCsv: return "csv";
    case FileKind::Vbo: return "vbo";
    case FileKind::Ubx: return "ubx";
    case FileKind::Imu: return "imu";
    case FileKind::Can: return "can";
    case FileKind::Laps: return "laps";
    case FileKind::Meta: return "meta";
    default: return "other";
  }
}

const char* baseName(const char* path) {
  const char* s = strrchr(path, '/');
  return s ? s + 1 : path;
}

// lt::json::Source over a file on the XTSD, one lock per read
class FileSource : public lt::json::Source {
 public:
  explicit FileSource(const char* path) {
    Lock lk;
    if (lk.ok()) f_ = g.fs->open(path, FILE_READ);
  }
  ~FileSource() override {
    Lock lk(10000);
    f_.close();
  }
  int read(uint8_t* buf, int cap) override {
    if (!f_) return -1;
    Lock lk;
    if (!lk.ok()) return -1;
    return static_cast<int>(f_.read(buf, static_cast<size_t>(cap)));
  }

 private:
  File f_;
};

// ---- pages ------------------------------------------------------------------------------------

void sendGz(const uint8_t* data, size_t len) {
  g.srv->sendHeader("Content-Encoding", "gzip");
  g.srv->sendHeader("Cache-Control", "no-store");
  g.srv->send_P(200, "text/html; charset=utf-8", reinterpret_cast<PGM_P>(data), len);
}

void handleIndex() { sendGz(assets::kIndexGz, assets::kIndexGzLen); }
void handleReportPage() { sendGz(assets::kReportGz, assets::kReportGzLen); }

// ---- JSON API ---------------------------------------------------------------------------------

void handleStatus() {
  Status s;
  statusFill(&s);
  char* b = g.sc->json;
  const int cap = static_cast<int>(sizeof g.sc->json);
  int len = 0;
  put(b, cap, &len, "{\"fw\":");
  putJsonString(b, cap, &len, s.fw);
  put(b, cap, &len, ",\"host\":");
  putJsonString(b, cap, &len, s.hostname);
  put(b, cap, &len, ",\"ip\":");
  putJsonString(b, cap, &len, s.ip);
  put(b, cap, &len, ",\"ssid\":");
  putJsonString(b, cap, &len, s.ssid);
  put(b, cap, &len, ",\"rssi\":");
  putInt(b, cap, &len, s.rssi);
  put(b, cap, &len, ",\"free_bytes\":");
  putInt(b, cap, &len, static_cast<int64_t>(s.freeBytes));
  put(b, cap, &len, ",\"total_bytes\":");
  putInt(b, cap, &len, static_cast<int64_t>(s.totalBytes));
  put(b, cap, &len, ",\"remain_h\":");
  if (s.remainHours >= 0.0f) putFloat(b, cap, &len, s.remainHours, 1);
  else put(b, cap, &len, "null");
  put(b, cap, &len, s.storageLow ? ",\"low\":true" : ",\"low\":false");
  put(b, cap, &len, ",\"utc_ms\":");
  putInt(b, cap, &len, s.utcMs);
  put(b, cap, &len, ",\"tz_min\":");
  putInt(b, cap, &len, s.tzMin);
  put(b, cap, &len, ",\"active\":");
  putJsonString(b, cap, &len, activeId());
  put(b, cap, &len, ",\"odo_km\":");
  if (s.odoKm >= 0.0f) putFloat(b, cap, &len, s.odoKm, 1);
  else put(b, cap, &len, "null");
  put(b, cap, &len, ",\"track_h\":");
  if (s.trackHours >= 0.0f) putFloat(b, cap, &len, s.trackHours, 2);
  else put(b, cap, &len, "null");
  put(b, cap, &len, ",\"uptime_s\":");
  putInt(b, cap, &len, static_cast<int64_t>(millis() / 1000));
  put(b, cap, &len, ",\"heap\":");
  putInt(b, cap, &len, static_cast<int64_t>(ESP.getFreeHeap()));
  put(b, cap, &len, ",\"last\":");
  putJsonString(b, cap, &len, lastText);
  put(b, cap, &len, storageMounted() ? ",\"storage_mounted\":true" : ",\"storage_mounted\":false");
  put(b, cap, &len, g.h.formatStorage ? ",\"can_format\":true}" : ",\"can_format\":false}");
  if (len < 0) return sendError(500, "internal", "status too long");
  sendJson(200, b);
}

// One session as JSON: {"id","meta","bytes","downloaded","report_dl","active","files":[...]}
bool sessionJson(Out& o, const char* id) {
  Scratch& sc = *g.sc;
  char dir[40];
  snprintf(dir, sizeof dir, "/sessions/%s", id);
  int nf = 0, nm = 0;
  uint64_t bytes = 0;
  int metaLen = 0;
  bool metaOk = false;
  {
    Lock lk;
    if (!lk.ok()) return false;
    File d = g.fs->open(dir, FILE_READ);
    if (!d || !d.isDirectory()) return false;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
      const char* name = baseName(f.name());
      if (f.isDirectory()) continue;
      const lt::webfmt::FileKind k = lt::webfmt::kindOf(name);
      if (k == lt::webfmt::FileKind::Marker) {
        if (nm < kMaxFiles) strlcpy(sc.markers[nm++], name + strlen(lt::webfmt::kMarkerPrefix), sizeof sc.markers[0]);
        continue;
      }
      if (k == lt::webfmt::FileKind::Hidden || nf >= kMaxFiles) continue;
      FileInfo& fi = sc.files[nf++];
      strlcpy(fi.name, name, sizeof fi.name);
      fi.size = static_cast<uint32_t>(f.size());
      fi.kind = static_cast<uint8_t>(k);
      fi.dl = false;
      bytes += fi.size;
    }
    d.close();
  }
  char metaPath[56];
  snprintf(metaPath, sizeof metaPath, "%s/meta.json", dir);
  if (readSmall(metaPath, sc.meta, kMaxMeta, &metaLen)) {
    lt::json::MemSource src(sc.meta, metaLen);
    metaOk = lt::json::check(src, nullptr);  // a meta.json cut by a power loss is shown as null
  }
  bool downloaded = false, reportDl = false;
  for (int m = 0; m < nm; ++m) {
    if (strcmp(sc.markers[m], lt::webfmt::kReportMarkerFile) == 0) reportDl = true;
    for (int i = 0; i < nf; ++i) {
      if (strcmp(sc.files[i].name, sc.markers[m]) == 0) {
        sc.files[i].dl = true;
        if (lt::webfmt::countsAsDownload(static_cast<lt::webfmt::FileKind>(sc.files[i].kind))) downloaded = true;
      }
    }
  }
  downloaded = downloaded || reportDl;
  char* b = sc.json;
  const int cap = static_cast<int>(sizeof sc.json);
  int len = 0;
  put(b, cap, &len, "{\"id\":");
  putJsonString(b, cap, &len, id);
  put(b, cap, &len, ",\"meta\":");
  o.print(b);
  if (metaOk) o.write(sc.meta, static_cast<size_t>(metaLen));
  else o.print("null");
  len = 0;
  put(b, cap, &len, ",\"bytes\":");
  putInt(b, cap, &len, static_cast<int64_t>(bytes));
  put(b, cap, &len, downloaded ? ",\"downloaded\":true" : ",\"downloaded\":false");
  put(b, cap, &len, reportDl ? ",\"report_dl\":true" : ",\"report_dl\":false");
  put(b, cap, &len, strcmp(id, activeId()) == 0 ? ",\"active\":true,\"files\":[" : ",\"active\":false,\"files\":[");
  o.print(b);
  for (int i = 0; i < nf; ++i) {
    len = 0;
    put(b, cap, &len, i ? ",{\"name\":" : "{\"name\":");
    putJsonString(b, cap, &len, sc.files[i].name);
    put(b, cap, &len, ",\"size\":");
    putInt(b, cap, &len, sc.files[i].size);
    put(b, cap, &len, ",\"kind\":\"");
    put(b, cap, &len, kindName(static_cast<lt::webfmt::FileKind>(sc.files[i].kind)));
    put(b, cap, &len, sc.files[i].dl ? "\",\"dl\":true}" : "\",\"dl\":false}");
    o.print(b);
  }
  return o.print("]}");
}

void handleSessions() {
  Scratch& sc = *g.sc;
  int n = 0;
  {
    Lock lk;
    if (!lk.ok()) return sendBusy();
    File d = g.fs->open("/sessions", FILE_READ);
    if (d && d.isDirectory()) {
      for (;;) {
        bool isDir = false;
        const String p = d.getNextFileName(&isDir);
        if (p.length() == 0) break;
        const char* name = baseName(p.c_str());
        if (isDir && lt::webfmt::isSessionId(name) && n < kMaxSessions) strlcpy(sc.ids[n++], name, sizeof sc.ids[0]);
      }
      d.close();
    }
  }
  // Newest first: the id is the local start time
  for (int i = 1; i < n; ++i) {
    char t[16];
    memcpy(t, sc.ids[i], sizeof t);
    int j = i - 1;
    while (j >= 0 && strcmp(sc.ids[j], t) < 0) {
      memcpy(sc.ids[j + 1], sc.ids[j], sizeof t);
      --j;
    }
    memcpy(sc.ids[j + 1], t, sizeof t);
  }
  Out o;
  if (!o.begin(200, "OK", "application/json; charset=utf-8")) return;
  o.print("[");
  bool first = true;
  for (int i = 0; i < n && o.ok(); ++i) {
    char id[16];
    memcpy(id, sc.ids[i], sizeof id);  // sessionJson uses the scratch buffers
    if (!first) o.print(",");
    if (sessionJson(o, id)) first = false;
    else if (!first) o.print("null");
  }
  o.print("]");
  o.end();
}

void handleSession() {
  const String id = g.srv->arg("id");
  if (!lt::webfmt::isSessionId(id.c_str())) return sendError(400, "bad-id", "セッションの指定が正しくありません");
  Out o;
  if (!o.begin(200, "OK", "application/json; charset=utf-8")) return;
  if (!sessionJson(o, id.c_str())) o.print("null");
  o.end();
}

void handleConfig() {
  static const char* const kNames[] = {"courses.json", "maint.json", "checklist.json", "wifi.json", "settings.json"};
  char* b = g.sc->json;
  const int cap = static_cast<int>(sizeof g.sc->json);
  int len = 0;
  put(b, cap, &len, "[");
  bool first = true;
  for (const char* name : kNames) {
    char path[40];
    snprintf(path, sizeof path, "/config/%s", name);
    bool exists = false;
    uint32_t size = 0;
    int64_t mtime = 0;
    {
      Lock lk;
      if (!lk.ok()) return sendBusy();
      File f = g.fs->open(path, FILE_READ);
      if (f && !f.isDirectory()) {
        exists = true;
        size = static_cast<uint32_t>(f.size());
        mtime = static_cast<int64_t>(f.getLastWrite());
      }
      f.close();
    }
    if (!exists && strcmp(name, "settings.json") == 0) continue;
    put(b, cap, &len, first ? "{\"name\":" : ",{\"name\":");
    first = false;
    putJsonString(b, cap, &len, name);
    put(b, cap, &len, exists ? ",\"exists\":true,\"size\":" : ",\"exists\":false,\"size\":");
    putInt(b, cap, &len, size);
    put(b, cap, &len, ",\"mtime\":");
    putInt(b, cap, &len, mtime);
    put(b, cap, &len, "}");
  }
  put(b, cap, &len, "]");
  if (len < 0) return sendError(500, "internal", "config list too long");
  sendJson(200, b);
}

// ---- downloads --------------------------------------------------------------------------------

void handleDl() {
  const String id = g.srv->arg("id");
  const String f = g.srv->arg("f");
  if (!lt::webfmt::isSessionId(id.c_str()) || !lt::webfmt::isFileName(f.c_str())) {
    return sendError(400, "bad-name", "ファイルの指定が正しくありません");
  }
  char path[112];
  snprintf(path, sizeof path, "/sessions/%s/%s", id.c_str(), f.c_str());
  File file;
  size_t size = 0;
  {
    Lock lk;
    if (!lk.ok()) return sendBusy();
    file = g.fs->open(path, FILE_READ);
    if (file && !file.isDirectory()) size = file.size();
  }
  if (!file || file.isDirectory()) {
    Lock lk(10000);
    file.close();
    return sendError(404, "not-found", "ファイルがありません");
  }
  const uint32_t t0 = millis();
  const bool all = sendFile(file, size, lt::webfmt::contentType(f.c_str()), f.c_str(), false);
  {
    Lock lk(10000);
    file.close();
  }
  if (all) {
    char marker[128];
    snprintf(marker, sizeof marker, "/sessions/%s/%s%s", id.c_str(), lt::webfmt::kMarkerPrefix, f.c_str());
    if (touch(marker)) notifySessions();
  }
  const uint32_t ms = millis() - t0;
  snprintf(lastText, sizeof lastText, "%s %s %.2f MB %.1f s %.2f MB/s", all ? "dl" : "dl ABORT", f.c_str(),
           static_cast<double>(size) / 1e6, ms / 1000.0, ms ? static_cast<double>(size) / 1e3 / ms : 0.0);
  log_i("web: %s", lastText);
}

// ---- delete -----------------------------------------------------------------------------------

void handleDelete() {
  const String id = g.srv->arg("id");
  if (!lt::webfmt::isSessionId(id.c_str())) return sendError(400, "bad-id", "セッションの指定が正しくありません");
  if (strcmp(id.c_str(), activeId()) == 0) return sendError(409, "active", "記録中のセッションは削除できません");
  char dir[40];
  snprintf(dir, sizeof dir, "/sessions/%s", id.c_str());
  int removed = 0;
  for (int round = 0; round < 8; ++round) {  // kMaxFiles names per round
    int n = 0;
    {
      Lock lk;
      if (!lk.ok()) return sendBusy();
      File d = g.fs->open(dir, FILE_READ);
      if (!d || !d.isDirectory()) {
        if (round == 0) return sendError(404, "not-found", "セッションがありません");
        break;
      }
      for (;;) {
        bool isDir = false;
        const String p = d.getNextFileName(&isDir);
        if (p.length() == 0 || n >= kMaxFiles) break;
        if (!isDir) strlcpy(g.sc->markers[n++], baseName(p.c_str()), sizeof g.sc->markers[0]);
      }
      d.close();
    }
    if (n == 0) break;
    for (int i = 0; i < n; ++i) {
      char path[112];
      snprintf(path, sizeof path, "%s/%s", dir, g.sc->markers[i]);
      Lock lk;
      if (!lk.ok()) return sendBusy();
      if (g.fs->remove(path)) ++removed;
    }
  }
  bool ok = false;
  {
    Lock lk;
    if (!lk.ok()) return sendBusy();
    ok = g.fs->rmdir(dir);
  }
  log_i("web: delete %s: %d files, folder %s", id.c_str(), removed, ok ? "removed" : "NOT removed");
  if (removed > 0 || ok) notifySessions();
  if (!ok) return sendError(500, "remove-failed", "フォルダを消せませんでした（中に残ったファイルがあります）");
  char b[64];
  snprintf(b, sizeof b, "{\"ok\":true,\"removed\":%d}", removed);
  sendJson(200, b);
}

// ---- maintenance ------------------------------------------------------------------------------

void handleOdo() {
  const String t = g.srv->arg("km");
  char* end = nullptr;
  const float km = t.length() > 0 && t.length() < 16 ? strtof(t.c_str(), &end) : -1.0f;
  if (!end || *end != 0 || !(km >= 0.0f && km <= 999999.9f)) return sendError(400, "bad-km", "ODO は 0〜999999.9 km の数で指定してください");
  if (!g.h.setOdometer) return sendError(501, "not-available", "本機にこの機能がありません");
  if (!g.h.setOdometer(km)) return sendError(409, "busy", "いまは ODO を変えられません（もう一度試してください）");
  sendJson(200, "{\"ok\":true}");
}

void handleMaintDone() {
  const String id = g.srv->arg("id");
  bool okId = id.length() > 0 && id.length() < 32;
  for (size_t i = 0; okId && i < id.length(); ++i) okId = isalnum(static_cast<unsigned char>(id[i])) || id[i] == '_';
  if (!okId) return sendError(400, "bad-id", "項目の指定が正しくありません");
  if (!g.h.maintDone) return sendError(501, "not-available", "本機にこの機能がありません");
  if (!g.h.maintDone(id.c_str())) return sendError(404, "not-found", "その項目を実施済みにできません（項目がないか、本機の日付が未設定）");
  sendJson(200, "{\"ok\":true}");
}

// ---- format (only when the XTSD has no usable FAT) -----------------------------------------------

void handleFormat() {
  if (!g.h.formatStorage || !g.h.storageMounted) return sendError(501, "not-available", "本機にこの機能がありません");
  if (g.h.storageMounted()) return sendError(409, "mounted", "XTSD は使える状態です。フォーマットしません");
  const bool ok = g.h.formatStorage();
  snprintf(lastText, sizeof lastText, "format: %s", ok ? "ok" : "FAILED");
  log_i("web: %s", lastText);
  if (!ok) return sendError(500, "format-failed", "フォーマットできませんでした");
  notifySessions();
  sendJson(200, "{\"ok\":true}");
}

// ---- uploads ----------------------------------------------------------------------------------

struct Up {
  bool active = false;
  bool vbo = false;
  char name[24] = "";
  const char* tmp = "";
  File f;
  size_t size = 0;
  size_t max = 0;
  const char* error = "";
};
Up up;

void closeUpload(bool removeTmp) {
  Lock lk(10000);
  if (up.f) up.f.close();
  if (removeTmp && up.tmp[0]) g.fs->remove(up.tmp);
}

}  // namespace

// ---- shared helpers ---------------------------------------------------------------------------

Lock::Lock(uint32_t ms) : ok_(!g.h.storageLock || g.h.storageLock(ms)) {}

Lock::~Lock() {
  if (ok_ && g.h.storageLock && g.h.storageUnlock) g.h.storageUnlock();
}

bool Out::begin(int code, const char* reason, const char* type, const char* extraHeaders) {
  c_ = &g.srv->client();
  chunked_ = g.srv->version() == "HTTP/1.1";
  n_ = 0;
  sent_ = 0;
  ok_ = true;
  char h[256];
  snprintf(h, sizeof h, "HTTP/1.%d %d %s\r\nContent-Type: %s\r\nCache-Control: no-store\r\nConnection: close\r\n%s",
           chunked_ ? 1 : 0, code, reason, type, chunked_ ? "Transfer-Encoding: chunked\r\n" : "");
  if (!raw(h, strlen(h))) return false;
  if (extraHeaders && !raw(extraHeaders, strlen(extraHeaders))) return false;
  return raw("\r\n", 2);
}

bool Out::raw(const void* p, size_t n) {
  if (!ok_) return false;
  if (c_->write(static_cast<const uint8_t*>(p), n) != n) ok_ = false;
  return ok_;
}

bool Out::flush() {
  if (!ok_ || n_ == 0) return ok_;
  if (chunked_) {
    char h[12];
    snprintf(h, sizeof h, "%X\r\n", static_cast<unsigned>(n_));
    raw(h, strlen(h));
  }
  raw(g.out, n_);
  if (chunked_) raw("\r\n", 2);
  if (ok_) sent_ += n_;
  n_ = 0;
  return ok_;
}

bool Out::write(const void* p, size_t n) {
  const uint8_t* s = static_cast<const uint8_t*>(p);
  while (n > 0 && ok_) {
    const size_t k = n < kOutCap - n_ ? n : kOutCap - n_;
    memcpy(g.out + n_, s, k);
    n_ += k;
    s += k;
    n -= k;
    if (n_ == kOutCap) flush();
  }
  return ok_;
}

bool Out::print(const char* s) { return write(s, strlen(s)); }

bool Out::end() {
  flush();
  if (chunked_) raw("0\r\n\r\n", 5);
  return ok_;
}

void sendJson(int code, const char* json) {
  g.srv->sendHeader("Cache-Control", "no-store");
  g.srv->send(code, "application/json; charset=utf-8", json);
}

void sendError(int code, const char* error, const char* message) {
  char b[400];
  int len = 0;
  put(b, sizeof b, &len, "{\"ok\":false,\"error\":");
  putJsonString(b, sizeof b, &len, error);
  put(b, sizeof b, &len, ",\"message\":");
  putJsonString(b, sizeof b, &len, message);
  put(b, sizeof b, &len, "}");
  sendJson(code, len > 0 ? b : "{\"ok\":false}");
}

void sendBusy() { sendError(503, "busy", "本機が XTSD を使用中です。少し待ってからもう一度"); }

const char* lastTransferText() { return lastText; }

void setLastTransfer(const char* text) { strlcpy(lastText, text, sizeof lastText); }

const char* activeId() {
  const char* a = g.h.activeSessionId ? g.h.activeSessionId() : nullptr;
  return a ? a : "";
}

bool storageMounted() { return !g.h.storageMounted || g.h.storageMounted(); }

void notifySessions() {
  if (g.h.sessionsChanged) g.h.sessionsChanged();
}

void statusFill(Status* s) {
  memset(s, 0, sizeof *s);
  strlcpy(s->hostname, "laptimer", sizeof s->hostname);
  strlcpy(s->ip, WiFi.localIP().toString().c_str(), sizeof s->ip);
  strlcpy(s->ssid, WiFi.SSID().c_str(), sizeof s->ssid);
  s->rssi = WiFi.RSSI();
  s->remainHours = -1.0f;
  s->tzMin = 540;
  s->odoKm = -1.0f;
  s->trackHours = -1.0f;
  if (g.h.status) g.h.status(s);
  s->fw[sizeof s->fw - 1] = '\0';
  s->hostname[sizeof s->hostname - 1] = '\0';
  s->ip[sizeof s->ip - 1] = '\0';
  s->ssid[sizeof s->ssid - 1] = '\0';
}

bool readSmall(const char* path, char* buf, int cap, int* len) {
  Lock lk;
  if (!lk.ok()) return false;
  File f = g.fs->open(path, FILE_READ);
  if (!f || f.isDirectory() || f.size() >= static_cast<size_t>(cap)) return false;
  *len = static_cast<int>(f.read(reinterpret_cast<uint8_t*>(buf), static_cast<size_t>(cap - 1)));
  buf[*len] = '\0';
  f.close();
  return *len > 0;
}

bool touch(const char* path) {
  Lock lk(10000);
  if (!lk.ok()) return false;
  File f = g.fs->open(path, FILE_WRITE);
  const bool ok = static_cast<bool>(f);
  f.close();
  return ok;
}

bool sendFile(File& f, size_t size, const char* type, const char* attachName, bool headOnly) {
  if (attachName) g.srv->sendHeader("Content-Disposition", String("attachment; filename=\"") + attachName + "\"");
  g.srv->sendHeader("Cache-Control", "no-store");
  g.srv->setContentLength(size);
  g.srv->send(200, type, "");
  if (headOnly) return true;
  NetworkClient& c = g.srv->client();
  size_t sent = 0;
  while (sent < size) {
    size_t n = 0;
    {
      Lock lk;
      if (!lk.ok()) break;
      n = f.read(g.buf, size - sent < kChunk ? size - sent : kChunk);
    }
    if (n == 0) break;
    const size_t w = c.write(g.buf, n);
    sent += w;
    if (w != n) break;
  }
  return sent == size;
}

bool isUploadUri(const String& uri) { return uri == "/upload" || uri == "/api/course_from_vbo"; }

// A POST to an upload route that is not multipart: forget any earlier, unfinished upload
void uploadReset() {
  if (up.active) closeUpload(true);
  up.active = false;
}

void uploadData(HTTPUpload& u) {
  if (u.status == UPLOAD_FILE_START) {
    if (up.active) closeUpload(true);  // a second file in the same form: only the first counts
    up = Up();
    up.active = true;
    up.vbo = g.srv->uri() == "/api/course_from_vbo";
    if (up.vbo) {
      strlcpy(up.name, "course.vbo", sizeof up.name);
      up.tmp = kCourseVboTmp;
      up.max = kMaxVboHead;
    } else {
      const String n = g.srv->arg("name");
      up.tmp = kUploadTmp;
      up.max = kMaxConfig;
      if (!lt::webfmt::isConfigName(n.c_str())) {
        up.error = "bad-name";
        return;
      }
      strlcpy(up.name, n.c_str(), sizeof up.name);
    }
    if (g.srv->arg("size").toInt() > static_cast<long>(up.max)) {
      up.error = "too-large";
      return;
    }
    Lock lk;
    if (!lk.ok()) {
      up.error = "busy";
      return;
    }
    g.fs->mkdir("/config");
    g.fs->remove(up.tmp);
    up.f = g.fs->open(up.tmp, FILE_WRITE);
    if (!up.f) up.error = "write-failed";
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (!up.active || up.error[0]) return;
    if (up.size + u.currentSize > up.max) {
      up.error = "too-large";
      return;
    }
    Lock lk;
    if (!lk.ok()) {
      up.error = "busy";
      return;
    }
    if (up.f.write(u.buf, u.currentSize) != u.currentSize) up.error = "write-failed";
    up.size += u.currentSize;
  } else if (u.status == UPLOAD_FILE_END) {
    if (!up.active) return;
    Lock lk(10000);
    up.f.close();
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    if (!up.active) return;
    closeUpload(true);
    log_i("web: upload %s aborted after %u bytes", up.name, static_cast<unsigned>(up.size));
    up.active = false;
  }
}

void uploadDone() {
  if (!up.active) return sendError(400, "no-file", "ファイルが届いていません");
  up.active = false;
  char* b = g.sc->json;
  const int cap = static_cast<int>(sizeof g.sc->json);
  int len = 0;
  if (up.error[0]) {
    closeUpload(true);
    put(b, cap, &len, "{\"ok\":false,\"error\":");
    putJsonString(b, cap, &len, up.error);
    put(b, cap, &len, ",\"name\":");
    putJsonString(b, cap, &len, up.name);
    put(b, cap, &len, ",\"size\":");
    putInt(b, cap, &len, static_cast<int64_t>(up.size));
    put(b, cap, &len, "}");
    snprintf(lastText, sizeof lastText, "upload %s refused: %s", up.name, up.error);
    return sendJson(200, b);
  }
  if (up.vbo) {
    char msg[160] = "";
    bool ok = false;
    if (g.h.courseFromVbo) ok = g.h.courseFromVbo(up.tmp, msg, sizeof msg);
    else strlcpy(msg, "本機にこの機能がありません", sizeof msg);
    msg[sizeof msg - 1] = '\0';
    closeUpload(true);
    if (ok && g.h.configChanged) g.h.configChanged("courses.json");
    put(b, cap, &len, ok ? "{\"ok\":true,\"error\":\"\",\"message\":" : "{\"ok\":false,\"error\":\"convert\",\"message\":");
    putJsonString(b, cap, &len, msg);
    put(b, cap, &len, "}");
    snprintf(lastText, sizeof lastText, "course from VBO: %s", ok ? "ok" : "refused");
    return sendJson(200, b);
  }
  // The file is checked where it lies; the old one is replaced only when the new one is good
  lt::json::Error e{0, 0, ""};
  bool good = false;
  {
    FileSource src(up.tmp);
    good = lt::json::check(src, &e);
  }
  if (good && strcmp(up.name, "courses.json") == 0) {
    FileSource src(up.tmp);
    good = lt::CourseDb::check(src, &e);
  } else if (good && strcmp(up.name, "maint.json") == 0) {
    FileSource src(up.tmp);
    good = lt::Maint::check(src, &e);
  }
  if (!good) {
    closeUpload(true);
    put(b, cap, &len, "{\"ok\":false,\"error\":\"json\",\"name\":");
    putJsonString(b, cap, &len, up.name);
    put(b, cap, &len, ",\"size\":");
    putInt(b, cap, &len, static_cast<int64_t>(up.size));
    put(b, cap, &len, ",\"line\":");
    putInt(b, cap, &len, e.line);
    put(b, cap, &len, ",\"col\":");
    putInt(b, cap, &len, e.col);
    put(b, cap, &len, ",\"why\":");
    putJsonString(b, cap, &len, e.why ? e.why : "");
    put(b, cap, &len, "}");
    snprintf(lastText, sizeof lastText, "upload %s refused: %s line %d col %d", up.name, e.why ? e.why : "", e.line, e.col);
    return sendJson(200, b);
  }
  char target[40], prev[48];
  snprintf(target, sizeof target, "/config/%s", up.name);
  snprintf(prev, sizeof prev, "/config/.prev_%s", up.name);
  bool replaced = false;
  {
    Lock lk(10000);
    if (lk.ok()) {
      g.fs->remove(prev);
      const bool had = g.fs->exists(target);
      if (had) g.fs->rename(target, prev);
      replaced = g.fs->rename(up.tmp, target);
      if (!replaced && had) g.fs->rename(prev, target);
      if (!replaced) g.fs->remove(up.tmp);
    }
  }
  if (!replaced) {
    snprintf(lastText, sizeof lastText, "upload %s: rename failed", up.name);
    return sendError(500, "rename-failed", "置き換えに失敗しました（本機の設定は変わっていません）");
  }
  if (g.h.configChanged) g.h.configChanged(up.name);
  put(b, cap, &len, "{\"ok\":true,\"name\":");
  putJsonString(b, cap, &len, up.name);
  put(b, cap, &len, ",\"size\":");
  putInt(b, cap, &len, static_cast<int64_t>(up.size));
  put(b, cap, &len, "}");
  snprintf(lastText, sizeof lastText, "upload %s stored (%u bytes)", up.name, static_cast<unsigned>(up.size));
  log_i("web: %s", lastText);
  sendJson(200, b);
}

}  // namespace detail

using namespace detail;

bool begin(fs::FS& fs, const Hooks& hooks) {
  end();
  g.fs = &fs;
  g.h = hooks;
  g.buf = static_cast<uint8_t*>(alloc(kChunk, false));
  g.out = static_cast<uint8_t*>(alloc(kOutCap, false));
  g.line = static_cast<char*>(alloc(kLineCap, false));
  g.fout = static_cast<char*>(alloc(kFilterOutCap, false));
  g.sc = static_cast<Scratch*>(alloc(sizeof(Scratch), true));
  g.srv = new (std::nothrow) WebServer(80);
  if (!g.buf || !g.out || !g.line || !g.fout || !g.sc || !g.srv) {
    log_e("web: out of memory");
    end();
    return false;
  }
  WebServer& s = *g.srv;
  s.on("/", HTTP_GET, handleIndex);
  s.on("/index.html", HTTP_GET, handleIndex);
  s.on("/maint", HTTP_GET, handleIndex);
  s.on("/report.html", HTTP_GET, handleReportPage);
  s.on("/report", HTTP_GET, handleReport);
  s.on("/api/reportdata", HTTP_GET, handleReportData);
  s.on("/dl", HTTP_GET, handleDl);
  s.on("/api/status", HTTP_GET, handleStatus);
  s.on("/api/sessions", HTTP_GET, handleSessions);
  s.on("/api/session", HTTP_GET, handleSession);
  s.on("/api/config", HTTP_GET, handleConfig);
  s.on("/api/delete", HTTP_POST, handleDelete);
  s.on("/api/maintdone", HTTP_POST, handleMaintDone);
  s.on("/api/odo", HTTP_POST, handleOdo);
  s.on("/api/format", HTTP_POST, handleFormat);
  RequestHandler* router = makeRouter();
  if (!router) {
    log_e("web: out of memory");
    end();
    return false;
  }
  s.addHandler(router);  // uploads, WebDAV, 404 - after every route above
  static const char* const kHeaders[] = {"Depth"};
  s.collectHeaders(const_cast<const char**>(kHeaders), 1);
  s.begin();
  log_i("web: listening on port 80");
  return true;
}

void handle() {
  if (g.srv) g.srv->handleClient();
}

void end() {
  if (g.srv) {
    g.srv->stop();
    delete g.srv;  // deletes the router too
    g.srv = nullptr;
  }
  heap_caps_free(g.buf);
  heap_caps_free(g.out);
  heap_caps_free(g.line);
  heap_caps_free(g.fout);
  heap_caps_free(g.sc);
  g.buf = g.out = nullptr;
  g.line = g.fout = nullptr;
  g.sc = nullptr;
}

bool running() { return g.srv != nullptr; }

const char* lastTransfer() { return detail::lastTransferText(); }

}  // namespace web
