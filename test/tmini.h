// test/tmini.h - minimal test runner for the host tests (go.sh test).
#pragma once

namespace tmini {

typedef void (*Fn)();
void add(const char* name, Fn fn);
void fail(const char* file, int line, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
void info(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

struct Reg {
  Reg(const char* name, Fn fn) { add(name, fn); }
};

}  // namespace tmini

#define TEST(name)                                 \
  static void name();                              \
  static const tmini::Reg name##_reg(#name, name); \
  static void name()

#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) tmini::fail(__FILE__, __LINE__, "%s", #cond);   \
  } while (0)

#define CHECKF(cond, ...)                                        \
  do {                                                           \
    if (!(cond)) tmini::fail(__FILE__, __LINE__, __VA_ARGS__);   \
  } while (0)
