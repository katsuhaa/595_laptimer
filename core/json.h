// core/json.h - JSON for the config files and the session files, without heap:
//  - check(): syntax check of a whole input with line/column and a reason code. Same rules, reason
//    codes and positions as JsonCheck in firmware/webcheck/webcheck.ino (upload check, P0-W W-5).
//  - Reader: pull reader for our files (objects, arrays, strings with \u escapes to UTF-8,
//    numbers parsed with integer arithmetic into float / int32 / int64 / 1e-7 degrees,
//    true/false/null). The caller provides the string buffers.
//  - Writer: JSON text into a caller buffer (escaping, exact decimal numbers, optional indent).
//  - Decimal text <-> scaled integers and floats without double (also used by the CSV/VBO code).
//
// Reason codes (check() and Reader): bad-bom, eof, unexpected, extra, expected-key, expected-colon,
// expected-comma-or-brace, expected-comma-or-bracket, eof-in-string, control-in-string, bad-escape,
// bad-number, too-deep, read-failed (the Source returned a negative count).
// Reader only: expected-object, expected-array, expected-string, expected-number, expected-bool,
// number-too-long, number-range, not-integer, not-in-object, not-in-array, no-value; loaders add
// their own codes with Reader::fail().
#pragma once

#include <cstdint>

namespace lt {
namespace json {

struct Source {
  virtual int read(uint8_t* buf, int cap) = 0;  // bytes read, 0 at end (negative: read error)
  virtual ~Source() {}
};

struct Error {
  int line;         // 1-based (0: no error)
  int col;          // 1-based, in characters: UTF-8 continuation bytes are not counted
  const char* why;  // reason code, "" when there is no error
};

constexpr int kMaxDepth = 32;  // nested objects/arrays

// The whole input is one JSON value (a UTF-8 BOM in front is allowed). err may be null.
// On success err is {0, 0, ""}; on failure it holds the position of the offending character.
bool check(Source& src, Error* err);

// Source over bytes in memory. chunk > 0: at most that many bytes per read() (tests).
class MemSource : public Source {
 public:
  MemSource(const char* text, int len = -1, int chunk = 0);  // len < 0: strlen(text)
  MemSource(const uint8_t* data, int len, int chunk = 0);
  int read(uint8_t* buf, int cap) override;
  void rewind() { pos_ = 0; }

 private:
  const uint8_t* p_;
  int n_;
  int pos_ = 0;
  int chunk_;
};

// Byte input with the line/column bookkeeping of webcheck's JsonCheck (shared by check() and Reader).
class Input {
 public:
  explicit Input(Source& src) : src_(&src) {}
  int peek();  // next byte, -1 at the end
  int get();   // consumes it
  int line() const { return line_; }
  int col() const { return col_; }  // position of the next character
  void resetCol() { col_ = 1; }
  bool readFailed() const { return readErr_; }

 private:
  Source* src_;
  uint8_t buf_[128];
  int n_ = 0;
  int i_ = 0;
  int line_ = 1;
  int col_ = 1;
  bool end_ = false;
  bool readErr_ = false;
};

inline int Input::peek() {
  if (i_ == n_) {
    if (end_) return -1;
    const int n = src_->read(buf_, static_cast<int>(sizeof buf_));
    i_ = 0;
    if (n <= 0) {
      n_ = 0;
      end_ = true;
      if (n < 0) readErr_ = true;
      return -1;
    }
    n_ = n > static_cast<int>(sizeof buf_) ? static_cast<int>(sizeof buf_) : n;
  }
  return buf_[i_];
}

inline int Input::get() {
  const int c = peek();
  if (c < 0) return c;
  ++i_;
  if (c == '\n') {
    ++line_;
    col_ = 1;
  } else if ((c & 0xC0) != 0x80) {  // UTF-8 continuation bytes do not start a character
    ++col_;
  }
  return c;
}

enum class Type : uint8_t { None, Object, Array, String, Number, Bool, Null };

// Pull reader. Usage:
//   json::Reader r(src);
//   if (!r.beginObject()) ...;
//   while (r.nextKey()) {                       // false at '}' (or on an error)
//     if (r.keyIs("version")) r.readInt(&v);
//     else if (r.keyIs("items")) { r.beginArray(); while (r.nextItem()) { ... } }
//     else r.skip();                             // a value left unread is skipped automatically
//   }
//   if (!r.finish()) -> r.error() has line, column and reason
// After the first error every call returns false (or Type::None / -1) and error() keeps the first one.
class Reader {
 public:
  explicit Reader(Source& src);

  Type peek();  // type of the next value without consuming it (None at the end or after an error)
  bool beginObject();
  bool nextKey();  // next member: true and key(); false at the end of the object (consumed)
  const char* key() const { return key_; }
  bool keyIs(const char* k) const;  // false for keys longer than kMaxKey - 1
  bool beginArray();
  bool nextItem();  // next element: true; false at the end of the array (consumed)

  // Each reads one whole value. readString returns the decoded length in bytes (without NUL);
  // when it is >= cap the text was cut at a UTF-8 character boundary (always NUL-terminated if cap > 0).
  // -1 on an error. cap 0 (buf may be null): only measures.
  int readString(char* buf, int cap);
  bool readFloat(float* v);
  bool readScaled(int64_t* v, int decimals);  // round(value * 10^decimals), half away from zero
  bool readInt(int32_t* v);                   // integral values only ("not-integer")
  bool readInt64(int64_t* v);
  bool readE7(int32_t* v);  // degrees -> 1e-7 degrees (exact decimal rounding)
  bool readBool(bool* v);
  bool skip();    // any one value (checked like check() does)
  bool finish();  // the top-level value is complete and only white space follows

  // Marks a content error (bad value, missing key ...) at the start of the last key or value read.
  // Always returns false.
  bool fail(const char* why);
  bool ok() const { return !err_.why[0]; }
  const Error& error() const { return err_; }
  int depth() const { return depth_; }

  static constexpr int kMaxKey = 48;
  static constexpr int kMaxNumber = 40;  // characters of one number

 private:
  bool start();
  void ws();
  bool syntax(const char* why);  // error at the current position
  bool valueAllowed();
  void mark();
  void valueDone();
  bool push(bool object);
  void pop();
  bool expectValueStart(int want, const char* whyWrong);
  int string(char* buf, int cap);
  bool number(char* text, int* n);
  bool word(const char* w);
  bool skipValue(int depth);
  bool skipPending();

  Input in_;
  Error err_{0, 0, ""};
  char key_[kMaxKey];
  bool keyLong_ = false;
  bool started_ = false;
  bool topDone_ = false;
  bool pending_ = false;  // nextKey()/nextItem() announced a value that is not read yet
  int depth_ = 0;
  uint32_t objMask_ = 0;    // bit d-1: level d is an object
  uint32_t firstMask_ = 0;  // bit d-1: no member/element read yet at level d
  int markLine_ = 1;
  int markCol_ = 1;
};

// JSON text into a caller buffer. Commas, quotes and escapes are handled; with pretty = true each
// member/element of a container starts on its own line (2 spaces per level) unless the container
// is flat (then "a, b, c" on one line). Overflow or misuse makes ok() false and finish() return 0.
class Writer {
 public:
  Writer(char* buf, int cap, bool pretty = true);
  Writer& beginObject(bool flat = false);
  Writer& endObject();
  Writer& beginArray(bool flat = false);
  Writer& endArray();
  Writer& key(const char* k);
  Writer& str(const char* s);                  // string value, escaped (UTF-8 kept as is)
  Writer& i64(int64_t v);
  Writer& scaled(int64_t v, int decimals);     // v / 10^decimals exactly: scaled(353949090, 7) = 35.3949090
  Writer& num(float v, int decimals, bool trim = true);  // rounded; trim: no trailing zeros; null if not finite
  Writer& boolean(bool v);
  Writer& null();
  int finish();  // length (a final '\n' when pretty), 0 on overflow or unbalanced containers
  bool ok() const { return ok_; }

 private:
  void raw(const char* s, int n);
  void ch(char c);
  void separator();
  void escaped(const char* s);
  bool push(bool object, bool flat);

  char* buf_;
  int cap_;
  int len_ = 0;
  bool ok_ = true;
  bool pretty_;
  bool afterKey_ = false;
  int depth_ = 0;
  uint32_t objMask_ = 0;
  uint32_t firstMask_ = 0;
  uint32_t flatMask_ = 0;
};

// ---- decimal text <-> numbers, integer arithmetic only ------------------------------------------
// Text form: [+-]digits[.digits][(e|E)[+-]digits] (a leading '+' and leading zeros are accepted
// here; the JSON reader checks the stricter JSON syntax before calling these). No white space.

// round(value * 10^decimals), half away from zero. false: not a number or outside int64.
// *exact (may be null): no digit was lost by the rounding.
bool parseScaled(const char* s, int n, int decimals, int64_t* out, bool* exact = nullptr);
bool parseFloat(const char* s, int n, float* out);  // false: not a number or not finite

// v / 10^decimals as text ("-0.005"); decimals 0..18. Length, or 0 when cap is too small.
int formatScaled(char* out, int cap, int64_t v, int decimals);
// round(v * 10^decimals) as above. 0 when cap is too small or v is not finite / too large.
int formatFloat(char* out, int cap, float v, int decimals);
// round(v * 10^decimals) half away from zero; false when not finite or outside int64.
bool roundScaled(float v, int decimals, int64_t* out);

}  // namespace json
}  // namespace lt
