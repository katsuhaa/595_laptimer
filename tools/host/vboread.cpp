#include "vboread.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace vboread {

namespace {

std::string trim(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  const size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

}  // namespace

bool read(const std::string& path, File* f, std::string* err) {
  *f = File();
  f->path = path;
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) {
    *err = "cannot open " + path;
    return false;
  }
  std::string section;
  std::string line;
  char buf[4096];
  bool first = true;
  while (std::fgets(buf, sizeof buf, fp)) {
    line = trim(buf);
    if (first) {
      f->created = line;
      first = false;
      continue;
    }
    if (line.empty()) continue;
    if (line[0] == '[') {
      section = line;
      if (section == "[data]") break;  // the samples are not needed here
      continue;
    }
    if (section == "[header]") {
      f->header.push_back(line);
    } else if (section == "[session data]" && line.compare(0, 5, "name ") == 0) {
      f->sessionName = trim(line.substr(5));
    } else if (section == "[laptiming]") {
      // "Kind lonA latA lonB latB <sep> name": the separator is not ASCII, so split on numbers
      std::istringstream in(line);
      Gate g;
      double v[4];
      if (!(in >> g.kind >> v[0] >> v[1] >> v[2] >> v[3])) continue;
      std::string rest;
      std::getline(in, rest);
      const size_t start = rest.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
      g.name = start == std::string::npos ? g.kind : trim(rest.substr(start));
      g.lonA = -v[0] / 60.0;
      g.latA = v[1] / 60.0;
      g.lonB = -v[2] / 60.0;
      g.latB = v[3] / 60.0;
      f->gates.push_back(g);
    }
  }
  std::fclose(fp);
  if (f->header.empty()) {
    *err = path + ": no [header] section";
    return false;
  }
  return true;
}

}  // namespace vboread
