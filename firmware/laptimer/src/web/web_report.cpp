// web_report.cpp - GET /report and /api/reportdata: web/report.html with the session's data put where
// its marker was (lt::report, the same blocks as the host `report` tool builds), streamed: the files
// are read in kChunk pieces, each under the storage lock, and the main log is decimated to ~10 Hz by
// lt::report::LogFilter on the way. Nothing is held in RAM beyond the fixed buffers of web::begin().
// The response is chunked (its length is known only at the end) and uncompressed (see embed.py).
#include <Arduino.h>

#include "lt_core.h"
#include "web_assets.h"
#include "web_internal.h"

namespace web {
namespace detail {

namespace {

struct Files {
  char meta[64];
  char laps[64];
  char main[64];
};

// The session's meta.json, laps and main log names; false when the folder is missing
bool findFiles(const char* dir, Files* f) {
  f->meta[0] = f->laps[0] = f->main[0] = '\0';
  Lock lk;
  if (!lk.ok()) return false;
  File d = g.fs->open(dir, FILE_READ);
  if (!d || !d.isDirectory()) return false;
  for (;;) {
    bool isDir = false;
    const String p = d.getNextFileName(&isDir);
    if (p.length() == 0) break;
    if (isDir) continue;
    const char* s = strrchr(p.c_str(), '/');
    const char* name = s ? s + 1 : p.c_str();
    switch (lt::webfmt::kindOf(name)) {
      case lt::webfmt::FileKind::Meta: strlcpy(f->meta, name, sizeof f->meta); break;
      case lt::webfmt::FileKind::Laps: strlcpy(f->laps, name, sizeof f->laps); break;
      case lt::webfmt::FileKind::MainCsv: strlcpy(f->main, name, sizeof f->main); break;
      default: break;
    }
  }
  d.close();
  return true;
}

// A small text file (meta.json, laps) into a script block, '<' escaped
bool streamEscaped(Out& o, const char* path) {
  File f;
  {
    Lock lk;
    if (!lk.ok()) return false;
    f = g.fs->open(path, FILE_READ);
  }
  bool ok = static_cast<bool>(f);
  while (ok) {
    size_t n = 0;
    {
      Lock lk;
      if (!lk.ok()) {
        ok = false;
        break;
      }
      n = f.read(g.buf, 1024);  // escaping can grow it six-fold: 6 KB < kFilterOutCap + kChunk
    }
    if (n == 0) break;
    size_t pos = 0;
    while (pos < n && ok) {
      int used = 0;
      const int k = lt::report::escapeForScript(reinterpret_cast<const char*>(g.buf) + pos, static_cast<int>(n - pos),
                                                g.fout, kFilterOutCap, &used);
      ok = o.write(g.fout, static_cast<size_t>(k));
      pos += static_cast<size_t>(used);
    }
  }
  Lock lk(10000);
  f.close();
  return ok;
}

// The main log through the report filter
bool streamLog(Out& o, const char* path, lt::report::FilterStats* st) {
  lt::report::LogFilter filter;
  filter.begin(g.line, kLineCap);
  File f;
  {
    Lock lk;
    if (!lk.ok()) return false;
    f = g.fs->open(path, FILE_READ);
  }
  bool ok = static_cast<bool>(f);
  int used = 0;
  while (ok) {
    size_t n = 0;
    {
      Lock lk;
      if (!lk.ok()) {
        ok = false;
        break;
      }
      n = f.read(g.buf, kChunk);
    }
    if (n == 0) break;
    int off = 0;
    while (off < static_cast<int>(n) && ok) {
      int produced = 0;
      off += filter.feed(reinterpret_cast<const char*>(g.buf) + off, static_cast<int>(n) - off, g.fout + used,
                         kFilterOutCap - used, &produced);
      used += produced;
      if (off < static_cast<int>(n) || kFilterOutCap - used < filter.maxOutput()) {
        ok = o.write(g.fout, static_cast<size_t>(used));
        used = 0;
      }
    }
  }
  filter.finish();
  if (ok && used > 0) ok = o.write(g.fout, static_cast<size_t>(used));
  {
    Lock lk(10000);
    f.close();
  }
  *st = filter.stats();
  return ok;
}

void report(bool page) {
  const String idArg = g.srv->arg("id");
  const char* id = idArg.c_str();
  if (!lt::webfmt::isSessionId(id)) return sendError(400, "bad-id", "セッションの指定が正しくありません");
  const bool save = page && g.srv->arg("save") == "1";
  char dir[40];
  snprintf(dir, sizeof dir, "/sessions/%s", id);
  Files files;
  if (!findFiles(dir, &files)) return sendError(404, "not-found", "セッションがありません");
  char extra[96] = "";
  if (save) snprintf(extra, sizeof extra, "Content-Disposition: attachment; filename=\"report_%s.html\"\r\n", id);
  const uint32_t t0 = millis();
  Out o;
  if (!o.begin(200, "OK", "text/html; charset=utf-8", extra)) return;
  if (page) o.write(assets::kReportHead, assets::kReportHeadLen);
  char path[112];
  o.print(lt::report::kMetaOpen);
  if (files.meta[0]) {
    snprintf(path, sizeof path, "%s/%s", dir, files.meta);
    streamEscaped(o, path);
  } else {
    o.print("null");
  }
  o.print(lt::report::kClose);
  o.print(lt::report::kLapsOpen);
  if (files.laps[0]) {
    snprintf(path, sizeof path, "%s/%s", dir, files.laps);
    streamEscaped(o, path);
  }
  o.print(lt::report::kClose);
  o.print(lt::report::kLogOpen);
  lt::report::FilterStats st;
  if (files.main[0]) {
    snprintf(path, sizeof path, "%s/%s", dir, files.main);
    streamLog(o, path, &st);
  }
  o.print(lt::report::kClose);
  Status s;
  statusFill(&s);
  char gen[40];
  snprintf(gen, sizeof gen, "595_laptimer %s", s.fw);
  lt::report::Info info;
  info.generator = gen;
  info.log = &st;
  const int n = lt::report::formatInfo(g.sc->json, sizeof g.sc->json, info);
  o.print(lt::report::kInfoOpen);
  if (n > 0) o.write(g.sc->json, static_cast<size_t>(n));
  else o.print("null");
  o.print(lt::report::kClose);
  if (page) o.write(assets::kReportTail, assets::kReportTailLen);
  const bool all = o.end();
  if (save && all) {
    char marker[64];
    snprintf(marker, sizeof marker, "%s/%s%s", dir, lt::webfmt::kMarkerPrefix, lt::webfmt::kReportMarkerFile);
    if (touch(marker)) notifySessions();
  }
  const uint32_t ms = millis() - t0;
  char text[112];
  snprintf(text, sizeof text, "%s %s %lld->%lld rows %.2f MB %.1f s", save ? "report save" : page ? "report" : "reportdata",
           all ? id : "ABORT", static_cast<long long>(st.rowsIn), static_cast<long long>(st.rowsOut),
           static_cast<double>(o.sent()) / 1e6, ms / 1000.0);
  setLastTransfer(text);
  log_i("web: %s", text);
}

}  // namespace

void handleReport() { report(true); }
void handleReportData() { report(false); }

}  // namespace detail
}  // namespace web
