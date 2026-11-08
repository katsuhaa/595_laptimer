#include "report_build.h"

#include <dirent.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rb {

namespace {

bool readAll(const std::string& path, std::string* out) {
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) return false;
  out->clear();
  char buf[65536];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) out->append(buf, n);
  std::fclose(fp);
  return true;
}

// Text for a script block, through lt::report::escapeForScript in small pieces
std::string scriptSafe(const std::string& s) {
  std::string o;
  char out[512];
  size_t pos = 0;
  while (pos < s.size()) {
    int used = 0;
    const int n = lt::report::escapeForScript(s.data() + pos, static_cast<int>(std::min<size_t>(s.size() - pos, 256)),
                                              out, sizeof out, &used);
    o.append(out, static_cast<size_t>(n));
    pos += static_cast<size_t>(used);
  }
  return o;
}

}  // namespace

bool loadTemplate(const std::string& path, Template* t, std::string* err) {
  std::string html;
  if (!readAll(path, &html)) {
    *err = "cannot read " + path;
    return false;
  }
  const size_t m = html.find(lt::report::kMarker);
  if (m == std::string::npos || html.find(lt::report::kMarker, m + 1) != std::string::npos) {
    *err = path + ": the marker " + std::string(lt::report::kMarker) + " must be there exactly once";
    return false;
  }
  t->head = html.substr(0, m);
  t->tail = html.substr(m + std::strlen(lt::report::kMarker));
  return true;
}

bool dataBlocks(const std::string& dir, const std::string& generator, const std::vector<std::string>& notes,
                Data* d, std::string* err) {
  *d = Data();
  DIR* dp = opendir(dir.c_str());
  if (!dp) {
    *err = "cannot open " + dir;
    return false;
  }
  while (dirent* e = readdir(dp)) {
    const char* n = e->d_name;
    switch (lt::webfmt::kindOf(n)) {
      case lt::webfmt::FileKind::Meta: d->metaName = n; break;
      case lt::webfmt::FileKind::Laps: d->lapsName = n; break;
      case lt::webfmt::FileKind::MainCsv: d->mainName = n; break;
      default: break;
    }
  }
  closedir(dp);
  std::string meta = "null", laps;
  if (!d->metaName.empty() && !readAll(dir + "/" + d->metaName, &meta)) meta = "null";
  if (!d->lapsName.empty()) readAll(dir + "/" + d->lapsName, &laps);

  std::string& o = d->blocks;
  o += lt::report::kMetaOpen + scriptSafe(meta) + lt::report::kClose;
  o += lt::report::kLapsOpen + scriptSafe(laps) + lt::report::kClose;
  o += lt::report::kLogOpen;
  lt::report::LogFilter f;
  char line[512];
  f.begin(line, sizeof line);
  if (!d->mainName.empty()) {
    FILE* fp = std::fopen((dir + "/" + d->mainName).c_str(), "rb");
    if (!fp) {
      *err = "cannot read " + d->mainName;
      return false;
    }
    char in[4096];
    char out[4096];
    int used = 0;
    size_t n = 0;
    while ((n = std::fread(in, 1, sizeof in, fp)) > 0) {
      int off = 0;
      while (off < static_cast<int>(n)) {
        int produced = 0;
        off += f.feed(in + off, static_cast<int>(n) - off, out + used, static_cast<int>(sizeof out) - used, &produced);
        used += produced;
        if (off < static_cast<int>(n) || static_cast<int>(sizeof out) - used < f.maxOutput()) {
          o.append(out, static_cast<size_t>(used));
          used = 0;
        }
      }
    }
    std::fclose(fp);
    f.finish();
    o.append(out, static_cast<size_t>(used));
  }
  o += lt::report::kClose;
  d->log = f.stats();
  std::vector<const char*> np;
  for (const std::string& s : notes) np.push_back(s.c_str());
  lt::report::Info info;
  info.generator = generator.c_str();
  info.log = &d->log;
  info.notes = np.data();
  info.numNotes = static_cast<int>(np.size());
  char buf[4096];
  const int n = lt::report::formatInfo(buf, sizeof buf, info);
  if (n < 0) {
    *err = "info block too long";
    return false;
  }
  o += lt::report::kInfoOpen + std::string(buf, static_cast<size_t>(n)) + lt::report::kClose;
  return true;
}

bool blockText(const std::string& html, const char* open, std::string* text) {
  const size_t a = html.find(open);
  if (a == std::string::npos || html.find(open, a + 1) != std::string::npos) return false;
  const size_t b = a + std::strlen(open);
  const size_t e = html.find("</script>", b);
  if (e == std::string::npos) return false;
  *text = html.substr(b, e - b);
  return true;
}

}  // namespace rb
