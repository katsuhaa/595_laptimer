// Runs every TEST() and prints one PASS/FAIL line per test and a RESULT line.
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "tmini.h"

namespace tmini {

namespace {

struct Case {
  const char* name;
  Fn fn;
};

std::vector<Case>& cases() {
  static std::vector<Case> v;
  return v;
}

int g_failures = 0;
std::vector<std::string> g_lines;  // details printed under the test's PASS/FAIL line

std::string vformat(const char* fmt, va_list ap) {
  char buf[512];
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  return buf;
}

}  // namespace

void add(const char* name, Fn fn) { cases().push_back({name, fn}); }

void fail(const char* file, int line, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const std::string msg = vformat(fmt, ap);
  va_end(ap);
  char where[256];
  std::snprintf(where, sizeof where, "  ! %s:%d: ", file, line);
  g_lines.push_back(where + msg);
  ++g_failures;
}

void info(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  g_lines.push_back("    " + vformat(fmt, ap));
  va_end(ap);
}

}  // namespace tmini

int main() {
  int failed = 0;
  const int total = static_cast<int>(tmini::cases().size());
  for (const auto& c : tmini::cases()) {
    tmini::g_failures = 0;
    tmini::g_lines.clear();
    c.fn();
    std::printf("%s %s\n", tmini::g_failures ? "FAIL" : "PASS", c.name);
    for (const auto& l : tmini::g_lines) std::printf("%s\n", l.c_str());
    if (tmini::g_failures) ++failed;
  }
  if (failed) {
    std::printf("RESULT: FAIL (%d of %d tests)\n", failed, total);
    return 1;
  }
  std::printf("RESULT: PASS (%d tests)\n", total);
  return 0;
}
