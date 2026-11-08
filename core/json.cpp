#include "json.h"

#include <cmath>
#include <cstring>

namespace lt {
namespace json {

namespace {

constexpr uint64_t kInt64Max = 9223372036854775807ull;

const float kPow10f[11] = {1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f, 1e6f, 1e7f, 1e8f, 1e9f, 1e10f};

bool isDigit(int c) { return c >= '0' && c <= '9'; }
bool isHex(int c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int hexValue(int c) { return isDigit(c) ? c - '0' : (c | 0x20) - 'a' + 10; }
bool isWs(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool isSimpleEscape(int e) {
  return e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' || e == 'r' || e == 't';
}
bool isValueStart(int c) {
  return c == '{' || c == '[' || c == '"' || c == 't' || c == 'f' || c == 'n' || c == '-' || isDigit(c);
}

// JsonCheck of firmware/webcheck/webcheck.ino, ported step by step (same order of reads, so the
// same reason and position). Only addition: a read error of the Source is reported as read-failed.
class Checker {
 public:
  explicit Checker(Source& s) : in_(s) {}

  bool run() {
    if (in_.peek() == 0xEF) {  // UTF-8 BOM
      in_.get();
      if (in_.get() != 0xBB || in_.get() != 0xBF) return fail("bad-bom");
      in_.resetCol();
    }
    if (!value()) return false;
    ws();
    if (in_.peek() >= 0) return fail("extra");
    if (in_.readFailed()) return fail("read-failed");
    return true;
  }
  Error error() const { return {line_, col_, why_}; }

 private:
  Input in_;
  int depth_ = 0;
  int line_ = 0;
  int col_ = 0;
  const char* why_ = "";

  bool fail(const char* why) {
    if (!*why_) {
      why_ = in_.readFailed() ? "read-failed" : why;
      line_ = in_.line();
      col_ = in_.col();
    }
    return false;
  }
  void ws() {
    for (int c = in_.peek(); isWs(c); c = in_.peek()) in_.get();
  }
  bool digits() {
    bool any = false;
    for (int c = in_.peek(); isDigit(c); c = in_.peek()) {
      in_.get();
      any = true;
    }
    return any;
  }
  bool word(const char* w) {
    for (; *w; ++w) {
      if (in_.peek() != *w) return fail(in_.peek() < 0 ? "eof" : "unexpected");
      in_.get();
    }
    return true;
  }
  bool string() {
    in_.get();  // the opening quote
    for (;;) {
      const int c = in_.peek();
      if (c < 0) return fail("eof-in-string");
      if (c == '"') {
        in_.get();
        return true;
      }
      if (c < 0x20) return fail("control-in-string");
      in_.get();
      if (c != '\\') continue;
      const int e = in_.peek();
      if (e == 'u') {
        in_.get();
        for (int k = 0; k < 4; ++k) {
          if (!isHex(in_.peek())) return fail("bad-escape");
          in_.get();
        }
      } else if (e > 0 && isSimpleEscape(e)) {
        in_.get();
      } else {
        return fail("bad-escape");
      }
    }
  }
  bool number() {
    if (in_.peek() == '-') in_.get();
    if (in_.peek() == '0') {
      in_.get();
    } else if (!digits()) {
      return fail("bad-number");
    }
    if (in_.peek() == '.') {
      in_.get();
      if (!digits()) return fail("bad-number");
    }
    if (in_.peek() == 'e' || in_.peek() == 'E') {
      in_.get();
      if (in_.peek() == '+' || in_.peek() == '-') in_.get();
      if (!digits()) return fail("bad-number");
    }
    return true;
  }
  bool value() {
    ws();
    const int c = in_.peek();
    if (c < 0) return fail("eof");
    if (c == '{' || c == '[') return container(c == '{');
    if (c == '"') return string();
    if (c == 't') return word("true");
    if (c == 'f') return word("false");
    if (c == 'n') return word("null");
    if (c == '-' || isDigit(c)) return number();
    return fail("unexpected");
  }
  bool container(bool object) {
    if (++depth_ > kMaxDepth) return fail("too-deep");
    const int close = object ? '}' : ']';
    in_.get();
    ws();
    if (in_.peek() == close) {
      in_.get();
      --depth_;
      return true;
    }
    for (;;) {
      if (object) {
        ws();
        if (in_.peek() != '"') return fail(in_.peek() < 0 ? "eof" : "expected-key");
        if (!string()) return false;
        ws();
        if (in_.peek() != ':') return fail(in_.peek() < 0 ? "eof" : "expected-colon");
        in_.get();
      }
      if (!value()) return false;
      ws();
      const int c = in_.peek();
      if (c == ',') {
        in_.get();
        continue;
      }
      if (c == close) {
        in_.get();
        --depth_;
        return true;
      }
      if (c < 0) return fail("eof");
      return fail(object ? "expected-comma-or-brace" : "expected-comma-or-bracket");
    }
  }
};

// Decoded string bytes into a caller buffer: a character is stored only when all of its bytes fit
// (one byte is kept for the NUL), and nothing is stored after the first one that did not fit.
struct Sink {
  char* buf;
  int cap;
  int len = 0;   // decoded bytes
  int w = 0;     // stored bytes
  bool full;
  bool keep = false;  // the current raw UTF-8 sequence is being stored

  Sink(char* b, int c) : buf(b), cap(c), full(c <= 0) {}

  void unit(const uint8_t* b, int n) {
    len += n;
    keep = false;
    if (full) return;
    if (w + n > cap - 1) {
      full = true;
      return;
    }
    for (int i = 0; i < n; ++i) buf[w++] = static_cast<char>(b[i]);
  }
  void rawByte(int c) {
    ++len;
    if ((c & 0xC0) == 0x80) {  // continuation: follows its lead byte
      if (keep && !full) {
        if (w < cap - 1) {
          buf[w++] = static_cast<char>(c);
        } else {
          full = true;
        }
      }
      return;
    }
    const int n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    keep = false;
    if (full) return;
    if (w + n > cap - 1) {
      full = true;
      return;
    }
    buf[w++] = static_cast<char>(c);
    keep = n > 1;
  }
  void code(int32_t cp) {
    uint8_t b[4];
    int n = 0;
    if (cp < 0x80) {
      b[n++] = static_cast<uint8_t>(cp);
    } else if (cp < 0x800) {
      b[n++] = static_cast<uint8_t>(0xC0 | (cp >> 6));
      b[n++] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      b[n++] = static_cast<uint8_t>(0xE0 | (cp >> 12));
      b[n++] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
      b[n++] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    } else {
      b[n++] = static_cast<uint8_t>(0xF0 | (cp >> 18));
      b[n++] = static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F));
      b[n++] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
      b[n++] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    }
    unit(b, n);
  }
  void close() {
    if (cap > 0) buf[w] = '\0';
  }
};

struct Decimal {
  bool neg = false;
  uint64_t m = 0;     // up to 19 significant digits
  int sig = 0;
  int64_t exp = 0;    // value = (m + lost digits) * 10^exp
  int lostFirst = -1;  // first digit that did not fit into m (-1: none)
  bool lostRest = false;  // a non-zero digit after it
};

void addDigit(Decimal* d, int v, bool frac) {
  if (d->sig == 0 && v == 0) {  // leading zero
    if (frac) --d->exp;
    return;
  }
  if (d->sig < 19) {
    d->m = d->m * 10u + static_cast<uint64_t>(v);
    ++d->sig;
    if (frac) --d->exp;
    return;
  }
  if (!frac) ++d->exp;
  if (d->lostFirst < 0) {
    d->lostFirst = v;
  } else if (v != 0) {
    d->lostRest = true;
  }
}

bool scanDecimal(const char* s, int n, Decimal* d) {
  if (!s || n <= 0) return false;
  int i = 0;
  if (s[i] == '+' || s[i] == '-') {
    d->neg = s[i] == '-';
    ++i;
  }
  bool any = false;
  for (; i < n && isDigit(s[i]); ++i) {
    any = true;
    addDigit(d, s[i] - '0', false);
  }
  if (i < n && s[i] == '.') {
    ++i;
    for (; i < n && isDigit(s[i]); ++i) {
      any = true;
      addDigit(d, s[i] - '0', true);
    }
  }
  if (!any) return false;
  if (i < n && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    bool eneg = false;
    if (i < n && (s[i] == '+' || s[i] == '-')) {
      eneg = s[i] == '-';
      ++i;
    }
    if (i >= n || !isDigit(s[i])) return false;
    int64_t e = 0;
    for (; i < n && isDigit(s[i]); ++i) {
      if (e < 1000000) e = e * 10 + (s[i] - '0');
    }
    d->exp += eneg ? -e : e;
  }
  return i == n;
}

}  // namespace

// ---- check ---------------------------------------------------------------------------------------

bool check(Source& src, Error* err) {
  Checker c(src);
  const bool ok = c.run();
  if (err) *err = ok ? Error{0, 0, ""} : c.error();
  return ok;
}

MemSource::MemSource(const char* text, int len, int chunk)
    : p_(reinterpret_cast<const uint8_t*>(text)),
      n_(len >= 0 ? len : (text ? static_cast<int>(std::strlen(text)) : 0)),
      chunk_(chunk) {}

MemSource::MemSource(const uint8_t* data, int len, int chunk) : p_(data), n_(len > 0 ? len : 0), chunk_(chunk) {}

int MemSource::read(uint8_t* buf, int cap) {
  int k = n_ - pos_;
  if (k > cap) k = cap;
  if (chunk_ > 0 && k > chunk_) k = chunk_;
  if (k <= 0) return 0;
  std::memcpy(buf, p_ + pos_, static_cast<size_t>(k));
  pos_ += k;
  return k;
}

// ---- Reader --------------------------------------------------------------------------------------

Reader::Reader(Source& src) : in_(src) { key_[0] = '\0'; }

bool Reader::start() {
  if (started_) return ok();
  started_ = true;
  if (in_.peek() == 0xEF) {  // UTF-8 BOM, as in check()
    in_.get();
    if (in_.get() != 0xBB || in_.get() != 0xBF) return syntax("bad-bom");
    in_.resetCol();
  }
  return true;
}

void Reader::ws() {
  for (int c = in_.peek(); isWs(c); c = in_.peek()) in_.get();
}

bool Reader::syntax(const char* why) {
  if (ok()) {
    err_.why = in_.readFailed() ? "read-failed" : why;
    err_.line = in_.line();
    err_.col = in_.col();
  }
  return false;
}

bool Reader::fail(const char* why) {
  if (ok()) {
    err_.why = why;
    err_.line = markLine_;
    err_.col = markCol_;
  }
  return false;
}

void Reader::mark() {
  markLine_ = in_.line();
  markCol_ = in_.col();
}

bool Reader::valueAllowed() {
  if (!start()) return false;
  if (depth_ == 0 ? !topDone_ : pending_) return true;
  return fail("no-value");  // read without nextKey()/nextItem(), or a second top-level value
}

void Reader::valueDone() {
  pending_ = false;
  if (depth_ == 0) topDone_ = true;
}

bool Reader::push(bool object) {
  if (depth_ >= kMaxDepth) return syntax("too-deep");
  in_.get();
  const uint32_t bit = 1u << depth_;
  ++depth_;
  if (object) {
    objMask_ |= bit;
  } else {
    objMask_ &= ~bit;
  }
  firstMask_ |= bit;
  pending_ = false;
  return true;
}

void Reader::pop() {
  in_.get();
  --depth_;
  pending_ = false;
  if (depth_ == 0) topDone_ = true;
}

bool Reader::expectValueStart(int want, const char* whyWrong) {
  ws();
  const int c = in_.peek();
  mark();
  if (c == want) return true;
  return syntax(c < 0 ? "eof" : isValueStart(c) ? whyWrong : "unexpected");
}

Type Reader::peek() {
  if (!start()) return Type::None;
  if (!(depth_ == 0 ? !topDone_ : pending_)) return Type::None;
  ws();
  const int c = in_.peek();
  if (c == '{') return Type::Object;
  if (c == '[') return Type::Array;
  if (c == '"') return Type::String;
  if (c == 't' || c == 'f') return Type::Bool;
  if (c == 'n') return Type::Null;
  if (c == '-' || isDigit(c)) return Type::Number;
  return Type::None;
}

bool Reader::beginObject() {
  if (!valueAllowed()) return false;
  if (!expectValueStart('{', "expected-object")) return false;
  return push(true);
}

bool Reader::beginArray() {
  if (!valueAllowed()) return false;
  if (!expectValueStart('[', "expected-array")) return false;
  return push(false);
}

bool Reader::skipPending() {
  if (!skipValue(depth_)) return false;
  valueDone();
  return true;
}

bool Reader::nextKey() {
  if (!start()) return false;
  if (depth_ == 0 || !(objMask_ & (1u << (depth_ - 1)))) return fail("not-in-object");
  if (pending_ && !skipPending()) return false;
  const uint32_t bit = 1u << (depth_ - 1);
  ws();
  int c = in_.peek();
  if (c == '}') {
    pop();
    return false;
  }
  if (!(firstMask_ & bit)) {
    if (c != ',') return syntax(c < 0 ? "eof" : "expected-comma-or-brace");
    in_.get();
    ws();
    c = in_.peek();
  }
  if (c != '"') return syntax(c < 0 ? "eof" : "expected-key");
  mark();
  const int n = string(key_, kMaxKey);
  if (n < 0) return false;
  keyLong_ = n >= kMaxKey;
  ws();
  c = in_.peek();
  if (c != ':') return syntax(c < 0 ? "eof" : "expected-colon");
  in_.get();
  firstMask_ &= ~bit;
  pending_ = true;
  return true;
}

bool Reader::nextItem() {
  if (!start()) return false;
  if (depth_ == 0 || (objMask_ & (1u << (depth_ - 1)))) return fail("not-in-array");
  if (pending_ && !skipPending()) return false;
  const uint32_t bit = 1u << (depth_ - 1);
  ws();
  const int c = in_.peek();
  if (c == ']') {
    pop();
    return false;
  }
  if (!(firstMask_ & bit)) {
    if (c != ',') return syntax(c < 0 ? "eof" : "expected-comma-or-bracket");
    in_.get();
  }
  firstMask_ &= ~bit;
  pending_ = true;
  return true;
}

bool Reader::keyIs(const char* k) const { return !keyLong_ && std::strcmp(key_, k) == 0; }

// At the opening quote. Returns the decoded length or -1 (error set).
int Reader::string(char* buf, int cap) {
  Sink out(buf, cap);
  in_.get();  // the opening quote
  int32_t high = -1;  // high surrogate waiting for its low half
  for (;;) {
    const int c = in_.peek();
    if (c < 0) {
      out.close();
      syntax("eof-in-string");
      return -1;
    }
    if (c == '"') {
      in_.get();
      break;
    }
    if (c < 0x20) {
      out.close();
      syntax("control-in-string");
      return -1;
    }
    in_.get();
    if (c != '\\') {
      if (high >= 0) {
        out.code(0xFFFD);
        high = -1;
      }
      out.rawByte(c);
      continue;
    }
    const int e = in_.peek();
    int32_t cp = 0;
    if (e == 'u') {
      in_.get();
      for (int k = 0; k < 4; ++k) {
        const int h = in_.peek();
        if (!isHex(h)) {
          out.close();
          syntax("bad-escape");
          return -1;
        }
        in_.get();
        cp = cp * 16 + hexValue(h);
      }
    } else if (e > 0 && isSimpleEscape(e)) {
      in_.get();
      cp = e == 'b' ? 8 : e == 'f' ? 12 : e == 'n' ? 10 : e == 'r' ? 13 : e == 't' ? 9 : e;
    } else {
      out.close();
      syntax("bad-escape");
      return -1;
    }
    if (high >= 0) {
      if (cp >= 0xDC00 && cp <= 0xDFFF) {
        out.code(0x10000 + ((high - 0xD800) << 10) + (cp - 0xDC00));
        high = -1;
        continue;
      }
      out.code(0xFFFD);  // a high surrogate without its low half
      high = -1;
    }
    if (cp >= 0xD800 && cp <= 0xDBFF) {
      high = cp;
      continue;
    }
    if (cp >= 0xDC00 && cp <= 0xDFFF) cp = 0xFFFD;  // a low half alone
    out.code(cp);
  }
  if (high >= 0) out.code(0xFFFD);
  out.close();
  return out.len;
}

// One number with the syntax of check(); text (kMaxNumber + 1 chars) may be null: only checked.
bool Reader::number(char* text, int* n) {
  int k = 0;
  bool good = true;
  auto take = [&]() {
    if (text) {
      if (k >= kMaxNumber) {
        good = syntax("number-too-long");
        return false;
      }
      text[k] = static_cast<char>(in_.peek());
    }
    in_.get();
    ++k;
    return true;
  };
  auto digits = [&]() {
    bool any = false;
    while (isDigit(in_.peek())) {
      if (!take()) return false;
      any = true;
    }
    if (!any && good) good = syntax("bad-number");
    return any;
  };
  if (in_.peek() == '-' && !take()) return false;
  if (in_.peek() == '0') {
    if (!take()) return false;
  } else if (!digits()) {
    return false;
  }
  if (in_.peek() == '.') {
    if (!take() || !digits()) return false;
  }
  if (in_.peek() == 'e' || in_.peek() == 'E') {
    if (!take()) return false;
    if ((in_.peek() == '+' || in_.peek() == '-') && !take()) return false;
    if (!digits()) return false;
  }
  if (text) text[k] = '\0';
  if (n) *n = k;
  return good;
}

bool Reader::word(const char* w) {
  for (; *w; ++w) {
    if (in_.peek() != *w) return syntax(in_.peek() < 0 ? "eof" : "unexpected");
    in_.get();
  }
  return true;
}

// Skips one value at nesting level `depth` with the rules and reason codes of check().
bool Reader::skipValue(int depth) {
  ws();
  const int c = in_.peek();
  mark();
  if (c < 0) return syntax("eof");
  if (c == '{' || c == '[') {
    if (depth + 1 > kMaxDepth) return syntax("too-deep");
    const bool object = c == '{';
    const int close = object ? '}' : ']';
    in_.get();
    ws();
    if (in_.peek() == close) {
      in_.get();
      return true;
    }
    for (;;) {
      if (object) {
        ws();
        if (in_.peek() != '"') return syntax(in_.peek() < 0 ? "eof" : "expected-key");
        if (string(nullptr, 0) < 0) return false;
        ws();
        if (in_.peek() != ':') return syntax(in_.peek() < 0 ? "eof" : "expected-colon");
        in_.get();
      }
      if (!skipValue(depth + 1)) return false;
      ws();
      const int d = in_.peek();
      if (d == ',') {
        in_.get();
        continue;
      }
      if (d == close) {
        in_.get();
        return true;
      }
      if (d < 0) return syntax("eof");
      return syntax(object ? "expected-comma-or-brace" : "expected-comma-or-bracket");
    }
  }
  if (c == '"') return string(nullptr, 0) >= 0;
  if (c == 't') return word("true");
  if (c == 'f') return word("false");
  if (c == 'n') return word("null");
  if (c == '-' || isDigit(c)) return number(nullptr, nullptr);
  return syntax("unexpected");
}

bool Reader::skip() {
  if (!valueAllowed()) return false;
  if (!skipValue(depth_)) return false;
  valueDone();
  return true;
}

int Reader::readString(char* buf, int cap) {
  if (cap > 0) buf[0] = '\0';
  if (!valueAllowed()) return -1;
  if (!expectValueStart('"', "expected-string")) return -1;
  const int n = string(buf, cap);
  if (n < 0) return -1;
  valueDone();
  return n;
}

bool Reader::readScaled(int64_t* v, int decimals) {
  if (!valueAllowed()) return false;
  ws();
  const int c = in_.peek();
  mark();
  if (c != '-' && !isDigit(c)) return syntax(c < 0 ? "eof" : isValueStart(c) ? "expected-number" : "unexpected");
  char t[kMaxNumber + 1];
  int n = 0;
  if (!number(t, &n)) return false;
  valueDone();
  if (!parseScaled(t, n, decimals, v)) return fail("number-range");
  return true;
}

bool Reader::readFloat(float* v) {
  if (!valueAllowed()) return false;
  ws();
  const int c = in_.peek();
  mark();
  if (c != '-' && !isDigit(c)) return syntax(c < 0 ? "eof" : isValueStart(c) ? "expected-number" : "unexpected");
  char t[kMaxNumber + 1];
  int n = 0;
  if (!number(t, &n)) return false;
  valueDone();
  if (!parseFloat(t, n, v)) return fail("number-range");
  return true;
}

bool Reader::readInt64(int64_t* v) {
  if (!valueAllowed()) return false;
  ws();
  const int c = in_.peek();
  mark();
  if (c != '-' && !isDigit(c)) return syntax(c < 0 ? "eof" : isValueStart(c) ? "expected-number" : "unexpected");
  char t[kMaxNumber + 1];
  int n = 0;
  if (!number(t, &n)) return false;
  valueDone();
  bool exact = false;
  if (!parseScaled(t, n, 0, v, &exact)) return fail("number-range");
  if (!exact) return fail("not-integer");
  return true;
}

bool Reader::readInt(int32_t* v) {
  int64_t x = 0;
  if (!readInt64(&x)) return false;
  if (x < -2147483647ll - 1 || x > 2147483647ll) return fail("number-range");
  *v = static_cast<int32_t>(x);
  return true;
}

bool Reader::readE7(int32_t* v) {
  int64_t x = 0;
  if (!readScaled(&x, 7)) return false;
  if (x < -2147483647ll - 1 || x > 2147483647ll) return fail("number-range");
  *v = static_cast<int32_t>(x);
  return true;
}

bool Reader::readBool(bool* v) {
  if (!valueAllowed()) return false;
  ws();
  const int c = in_.peek();
  mark();
  if (c == 't') {
    if (!word("true")) return false;
    *v = true;
  } else if (c == 'f') {
    if (!word("false")) return false;
    *v = false;
  } else {
    return syntax(c < 0 ? "eof" : isValueStart(c) ? "expected-bool" : "unexpected");
  }
  valueDone();
  return true;
}

bool Reader::finish() {
  if (!start()) return false;
  while (depth_ > 0) {  // the rest of every open container (values not read are checked and skipped)
    const bool object = (objMask_ & (1u << (depth_ - 1))) != 0;
    if (object ? nextKey() : nextItem()) continue;
    if (!ok()) return false;
  }
  if (!topDone_ && !skip()) return false;
  ws();
  if (in_.peek() >= 0) return syntax("extra");
  if (in_.readFailed()) return syntax("read-failed");
  return true;
}

// ---- Writer --------------------------------------------------------------------------------------

Writer::Writer(char* buf, int cap, bool pretty) : buf_(buf), cap_(cap), pretty_(pretty) {
  if (cap_ > 0) {
    buf_[0] = '\0';
  } else {
    ok_ = false;
  }
}

void Writer::raw(const char* s, int n) {
  if (!ok_) return;
  if (len_ + n >= cap_) {  // room for the NUL
    ok_ = false;
    return;
  }
  std::memcpy(buf_ + len_, s, static_cast<size_t>(n));
  len_ += n;
  buf_[len_] = '\0';
}

void Writer::ch(char c) { raw(&c, 1); }

// Before a key, or before a value that has no key (array element or the top-level value)
void Writer::separator() {
  if (depth_ == 0) {
    if (len_ > 0) ok_ = false;  // a second top-level value
    return;
  }
  const uint32_t bit = 1u << (depth_ - 1);
  const bool first = (firstMask_ & bit) != 0;
  if (!first) ch(',');
  firstMask_ &= ~bit;
  if (!pretty_) return;
  if (flatMask_ & bit) {
    if (!first) ch(' ');
    return;
  }
  ch('\n');
  for (int i = 0; i < depth_; ++i) raw("  ", 2);
}

void Writer::escaped(const char* s) {
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
    const unsigned char c = *p;
    if (c == '"' || c == '\\') {
      const char e[2] = {'\\', static_cast<char>(c)};
      raw(e, 2);
    } else if (c == '\n') {
      raw("\\n", 2);
    } else if (c == '\r') {
      raw("\\r", 2);
    } else if (c == '\t') {
      raw("\\t", 2);
    } else if (c < 0x20) {
      const char* hex = "0123456789abcdef";
      const char e[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
      raw(e, 6);
    } else {
      ch(static_cast<char>(c));
    }
  }
}

bool Writer::push(bool object, bool flat) {
  if (depth_ >= kMaxDepth) {
    ok_ = false;
    return false;
  }
  const uint32_t bit = 1u << depth_;
  const bool parentFlat = depth_ > 0 && (flatMask_ & (1u << (depth_ - 1)));
  ++depth_;
  if (object) {
    objMask_ |= bit;
  } else {
    objMask_ &= ~bit;
  }
  firstMask_ |= bit;
  if (flat || parentFlat) {
    flatMask_ |= bit;
  } else {
    flatMask_ &= ~bit;
  }
  return true;
}

// A value: after key() in an object, otherwise an array element or the top-level value
#define LT_JSON_VALUE_START()                                            \
  do {                                                                   \
    if (!ok_) return *this;                                              \
    if (afterKey_) {                                                     \
      afterKey_ = false;                                                 \
    } else {                                                             \
      if (depth_ > 0 && (objMask_ & (1u << (depth_ - 1)))) ok_ = false;  \
      separator();                                                       \
    }                                                                    \
  } while (0)

Writer& Writer::beginObject(bool flat) {
  LT_JSON_VALUE_START();
  ch('{');
  push(true, flat);
  return *this;
}

Writer& Writer::beginArray(bool flat) {
  LT_JSON_VALUE_START();
  ch('[');
  push(false, flat);
  return *this;
}

Writer& Writer::endObject() {
  if (!ok_) return *this;
  if (depth_ == 0 || afterKey_ || !(objMask_ & (1u << (depth_ - 1)))) {
    ok_ = false;
    return *this;
  }
  const uint32_t bit = 1u << (depth_ - 1);
  if (pretty_ && !(firstMask_ & bit) && !(flatMask_ & bit)) {
    ch('\n');
    for (int i = 0; i < depth_ - 1; ++i) raw("  ", 2);
  }
  ch('}');
  --depth_;
  return *this;
}

Writer& Writer::endArray() {
  if (!ok_) return *this;
  if (depth_ == 0 || (objMask_ & (1u << (depth_ - 1)))) {
    ok_ = false;
    return *this;
  }
  const uint32_t bit = 1u << (depth_ - 1);
  if (pretty_ && !(firstMask_ & bit) && !(flatMask_ & bit)) {
    ch('\n');
    for (int i = 0; i < depth_ - 1; ++i) raw("  ", 2);
  }
  ch(']');
  --depth_;
  return *this;
}

Writer& Writer::key(const char* k) {
  if (!ok_) return *this;
  if (depth_ == 0 || afterKey_ || !(objMask_ & (1u << (depth_ - 1)))) {
    ok_ = false;
    return *this;
  }
  separator();
  ch('"');
  escaped(k);
  ch('"');
  ch(':');
  if (pretty_) ch(' ');
  afterKey_ = true;
  return *this;
}

Writer& Writer::str(const char* s) {
  LT_JSON_VALUE_START();
  ch('"');
  escaped(s ? s : "");
  ch('"');
  return *this;
}

Writer& Writer::i64(int64_t v) { return scaled(v, 0); }

Writer& Writer::scaled(int64_t v, int decimals) {
  LT_JSON_VALUE_START();
  char t[32];
  const int n = formatScaled(t, sizeof t, v, decimals);
  if (n <= 0) {
    ok_ = false;
    return *this;
  }
  raw(t, n);
  return *this;
}

Writer& Writer::num(float v, int decimals, bool trim) {
  if (!std::isfinite(v)) return null();
  LT_JSON_VALUE_START();
  char t[32];
  int n = formatFloat(t, sizeof t, v, decimals);
  if (n <= 0) {
    ok_ = false;
    return *this;
  }
  if (trim && decimals > 0) {
    while (t[n - 1] == '0') --n;
    if (t[n - 1] == '.') --n;
    if (n == 2 && t[0] == '-' && t[1] == '0') {  // "-0.00" trimmed
      t[0] = '0';
      n = 1;
    }
  }
  raw(t, n);
  return *this;
}

Writer& Writer::boolean(bool v) {
  LT_JSON_VALUE_START();
  if (v) {
    raw("true", 4);
  } else {
    raw("false", 5);
  }
  return *this;
}

Writer& Writer::null() {
  LT_JSON_VALUE_START();
  raw("null", 4);
  return *this;
}

#undef LT_JSON_VALUE_START

int Writer::finish() {
  if (!ok_ || depth_ != 0 || afterKey_ || len_ == 0) return 0;
  if (pretty_) ch('\n');
  return ok_ ? len_ : 0;
}

// ---- decimal text <-> numbers --------------------------------------------------------------------

bool parseScaled(const char* s, int n, int decimals, int64_t* out, bool* exact) {
  Decimal d;
  if (decimals < 0 || decimals > 18 || !scanDecimal(s, n, &d)) return false;
  uint64_t q = 0;
  bool ex = true;
  const int64_t e = d.exp + decimals;
  const bool lostZero = d.lostFirst <= 0 && !d.lostRest;
  if (d.m == 0) {
    q = 0;
  } else if (e >= 0) {
    if (e > 19) return false;
    q = d.m;
    for (int64_t k = 0; k < e; ++k) {
      if (q > kInt64Max / 10u) return false;
      q *= 10u;
    }
    // digits were lost only when m has 19 digits; then e > 0 has overflowed above
    if (d.lostFirst >= 5) ++q;
    ex = lostZero;
  } else if (e < -19) {
    q = 0;  // m < 10^19: below 0.1 after scaling
    ex = false;
  } else {
    uint64_t p = 1;
    for (int64_t k = 0; k < -e; ++k) p *= 10u;
    q = d.m / p;
    const uint64_t r = d.m % p;
    if (r >= p / 2u) ++q;  // half away from zero (lost digits cannot change it: r is an integer)
    ex = r == 0 && lostZero;
  }
  if (q > kInt64Max) return false;
  *out = d.neg ? -static_cast<int64_t>(q) : static_cast<int64_t>(q);
  if (exact) *exact = ex;
  return true;
}

bool parseFloat(const char* s, int n, float* out) {
  Decimal d;
  if (!scanDecimal(s, n, &d)) return false;
  float f = static_cast<float>(d.m);
  int64_t e = d.exp;
  if (d.m != 0) {
    if (e > 60) return false;
    if (e < -90) {
      f = 0.0f;
    } else {
      while (e > 10) {
        f *= 1e10f;
        e -= 10;
      }
      while (e < -10) {
        f /= 1e10f;
        e += 10;
      }
      f = e >= 0 ? f * kPow10f[e] : f / kPow10f[-e];
    }
  }
  if (!std::isfinite(f)) return false;
  *out = d.neg ? -f : f;
  return true;
}

int formatScaled(char* out, int cap, int64_t v, int decimals) {
  if (decimals < 0 || decimals > 18 || cap <= 0) return 0;
  char d[24];
  uint64_t u = v < 0 ? 0u - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
  int n = 0;
  do {
    d[n++] = static_cast<char>('0' + static_cast<int>(u % 10u));
    u /= 10u;
  } while (u != 0);
  while (n <= decimals) d[n++] = '0';  // at least one digit before the point
  const int len = (v < 0 ? 1 : 0) + n + (decimals > 0 ? 1 : 0);
  if (len + 1 > cap) return 0;
  int k = 0;
  if (v < 0) out[k++] = '-';
  for (int i = n - 1; i >= 0; --i) {
    out[k++] = d[i];
    if (i == decimals && decimals > 0) out[k++] = '.';
  }
  out[k] = '\0';
  return k;
}

bool roundScaled(float v, int decimals, int64_t* out) {
  if (!std::isfinite(v) || decimals < 0 || decimals > 10) return false;
  const float x = v * kPow10f[decimals];
  if (!(x > -9.2e18f && x < 9.2e18f)) return false;
  *out = std::llround(x);
  return true;
}

int formatFloat(char* out, int cap, float v, int decimals) {
  int64_t q = 0;
  if (!roundScaled(v, decimals, &q)) return 0;
  return formatScaled(out, cap, q, decimals);
}

}  // namespace json
}  // namespace lt
