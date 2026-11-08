// web_dav.cpp - the catch-all request handler, added after every WebServer::on() route:
//  - POST /upload and /api/course_from_vbo (multipart): handed to uploadData()/uploadDone() in web.cpp.
//    The HTTPUpload comes as an argument, so a request that is not multipart cannot reach a missing
//    upload object; its body is read and dropped (raw).
//  - read-only WebDAV class 1 for the Mac Finder: OPTIONS, PROPFIND Depth 0/1, GET/HEAD of files under
//    /sessions, /best, /config, /replay. Hidden: names starting with '.', wifi.json. Writes: 403/405.
//    A complete WebDAV GET does not write a download marker (the Finder also reads for previews).
//  - everything else: 404.
#include <Arduino.h>

#include <new>

#include "lt_core.h"
#include "web_internal.h"

namespace web {
namespace detail {

namespace {

constexpr int kMaxDepth = 3;  // /sessions/<id>/<file>
const char* const kTop[] = {"sessions", "best", "config", "replay"};

struct DavPath {
  char seg[kMaxDepth][64];
  int n = 0;
  char path[3 * 64 + 4];  // on the fs
};

bool visibleName(int level, const char* parent, const char* name) {
  if (level == 0) {
    for (const char* t : kTop) {
      if (strcmp(name, t) == 0) return true;
    }
    return false;
  }
  if (!lt::webfmt::isFileName(name)) return false;  // also every name starting with '.'
  return !(level == 1 && strcmp(parent, "config") == 0 && strcmp(name, "wifi.json") == 0);
}

// "/sessions/20261108_103000/" -> segments; false for anything hidden or malformed
bool parse(const String& uri, DavPath* p) {
  const String d = WebServer::urlDecode(uri);
  p->n = 0;
  size_t i = 0;
  while (i < d.length()) {
    while (i < d.length() && d[i] == '/') ++i;
    if (i >= d.length()) break;
    const size_t start = i;
    while (i < d.length() && d[i] != '/') ++i;
    if (p->n >= kMaxDepth || i - start >= sizeof p->seg[0]) return false;
    memcpy(p->seg[p->n], d.c_str() + start, i - start);
    p->seg[p->n][i - start] = '\0';
    if (!visibleName(p->n, p->n ? p->seg[p->n - 1] : "", p->seg[p->n])) return false;
    ++p->n;
  }
  int len = 0;
  p->path[0] = '/';
  p->path[1] = '\0';
  for (int k = 0; k < p->n; ++k) {
    len += snprintf(p->path + len, sizeof p->path - static_cast<size_t>(len), "/%s", p->seg[k]);
  }
  return true;
}

void notFound() { g.srv->send(404, "text/plain; charset=utf-8", "not found"); }

void options() {
  g.srv->sendHeader("DAV", "1");
  g.srv->sendHeader("Allow", "OPTIONS, GET, HEAD, PROPFIND");
  g.srv->sendHeader("MS-Author-Via", "DAV");
  g.srv->send(200, "text/plain", "");
}

bool entry(Out& o, const lt::webfmt::DavEntry& e) {
  const int n = lt::webfmt::davResponse(g.sc->json, sizeof g.sc->json, e);
  return n > 0 && o.write(g.sc->json, static_cast<size_t>(n));
}

void propfind(const String& uri) {
  DavPath p;
  if (!parse(uri, &p)) return notFound();
  const String depth = g.srv->header("Depth");
  // What is it: the root, a folder or a file
  bool isDir = p.n == 0;
  uint32_t size = 0;
  int64_t mtime = 0;
  if (p.n > 0) {
    Lock lk;
    if (!lk.ok()) return sendBusy();
    File f = g.fs->open(p.path, FILE_READ);
    if (!f) return notFound();
    isDir = f.isDirectory();
    size = isDir ? 0 : static_cast<uint32_t>(f.size());
    mtime = static_cast<int64_t>(f.getLastWrite());
    f.close();
  }
  char href[3 * 64 + 8];
  snprintf(href, sizeof href, "%s%s", p.n ? p.path : "", isDir ? "/" : "");
  Out o;
  if (!o.begin(207, "Multi-Status", "application/xml; charset=\"utf-8\"")) return;
  o.print(lt::webfmt::kDavHead);
  lt::webfmt::DavEntry self;
  self.href = href;
  self.name = p.n ? p.seg[p.n - 1] : "laptimer";
  self.collection = isDir;
  self.size = size;
  self.mtimeS = mtime;
  if (isDir) {
    Status s;
    statusFill(&s);
    if (s.totalBytes > 0) {
      self.quotaFree = static_cast<int64_t>(s.freeBytes);
      self.quotaUsed = static_cast<int64_t>(s.totalBytes - s.freeBytes);
    }
  }
  entry(o, self);
  if (isDir && depth != "0" && p.n < kMaxDepth) {
    File d;
    {
      Lock lk;
      if (lk.ok()) d = g.fs->open(p.n ? p.path : "/", FILE_READ);
    }
    while (d && o.ok()) {
      char name[64];
      bool childDir = false;
      uint32_t childSize = 0;
      int64_t childTime = 0;
      {
        Lock lk;
        if (!lk.ok()) break;
        File f = d.openNextFile();
        if (!f) break;
        const char* full = f.name();
        const char* slash = strrchr(full, '/');
        strlcpy(name, slash ? slash + 1 : full, sizeof name);
        childDir = f.isDirectory();
        childSize = childDir ? 0 : static_cast<uint32_t>(f.size());
        childTime = static_cast<int64_t>(f.getLastWrite());
        f.close();
      }
      if (!visibleName(p.n, p.n ? p.seg[p.n - 1] : "", name)) continue;
      char child[3 * 64 + 72];
      snprintf(child, sizeof child, "%s%s%s", href, name, childDir ? "/" : "");
      lt::webfmt::DavEntry e;
      e.href = child;
      e.name = name;
      e.collection = childDir;
      e.size = childSize;
      e.mtimeS = childTime;
      entry(o, e);
    }
    Lock lk(10000);
    d.close();
  }
  o.print(lt::webfmt::kDavTail);
  o.end();
}

void getFile(const String& uri, bool headOnly) {
  if (uri == "/config/wifi.json") return sendError(403, "secret", "wifi.json はパスワードを含むため本機から読み出せません");
  DavPath p;
  if (!parse(uri, &p) || p.n < 2) return notFound();
  File f;
  size_t size = 0;
  {
    Lock lk;
    if (!lk.ok()) return sendBusy();
    f = g.fs->open(p.path, FILE_READ);
    if (f && !f.isDirectory()) size = f.size();
  }
  if (!f || f.isDirectory()) {
    Lock lk(10000);
    f.close();
    return notFound();
  }
  const char* name = p.seg[p.n - 1];
  const bool save = g.srv->arg("save") == "1";
  const uint32_t t0 = millis();
  const bool all = sendFile(f, size, lt::webfmt::contentType(name), save ? name : nullptr, headOnly);
  {
    Lock lk(10000);
    f.close();
  }
  if (!headOnly && size >= 100000) {
    const uint32_t ms = millis() - t0;
    char text[112];
    snprintf(text, sizeof text, "%s %s %.2f MB %.1f s", all ? "get" : "get ABORT", name, static_cast<double>(size) / 1e6,
             ms / 1000.0);
    setLastTransfer(text);
  }
}

void route(WebServer& s, HTTPMethod m, const String& uri) {
  if (m == HTTP_POST && isUploadUri(uri)) return uploadDone();
  switch (m) {
    case HTTP_OPTIONS: return options();
    case HTTP_PROPFIND: return propfind(uri);
    case HTTP_GET: return getFile(uri, false);
    case HTTP_HEAD: return getFile(uri, true);
    case HTTP_LOCK:
    case HTTP_UNLOCK:
      s.sendHeader("Allow", "OPTIONS, GET, HEAD, PROPFIND");
      return s.send(405, "text/plain", "read-only");
    case HTTP_PUT:
    case HTTP_DELETE:
    case HTTP_MKCOL:
    case HTTP_MOVE:
    case HTTP_COPY:
    case HTTP_PROPPATCH:
    case HTTP_PATCH:
    case HTTP_POST:
      return s.send(403, "text/plain", "read-only");
    default:
      s.sendHeader("Allow", "OPTIONS, GET, HEAD, PROPFIND");
      return s.send(405, "text/plain", "not supported");
  }
}

class Router : public RequestHandler {
 public:
  bool canHandle(WebServer& server, HTTPMethod method, const String& uri) override {
    (void)server;
    (void)method;
    (void)uri;
    return true;  // last in the list: everything the routes did not take
  }
  bool canUpload(WebServer& server, const String& uri) override {
    return server.method() == HTTP_POST && isUploadUri(uri);
  }
  bool canRaw(WebServer& server, const String& uri) override {
    (void)server;
    (void)uri;
    return true;  // a body that is not multipart (PUT ...) is read in pieces and dropped
  }
  void upload(WebServer& server, const String& uri, HTTPUpload& u) override {
    (void)server;
    if (isUploadUri(uri)) uploadData(u);
  }
  void raw(WebServer& server, const String& uri, HTTPRaw& r) override {
    (void)server;
    if (r.status == RAW_START && isUploadUri(uri)) uploadReset();  // not multipart: no file
  }
  bool handle(WebServer& server, HTTPMethod method, const String& uri) override {
    route(server, method, uri);
    return true;
  }
};

}  // namespace

RequestHandler* makeRouter() { return new (std::nothrow) Router(); }

}  // namespace detail
}  // namespace web
