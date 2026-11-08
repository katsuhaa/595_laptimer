// core/json: syntax check (same reasons/positions as webcheck's JsonCheck), pull reader, writer,
// decimal conversions.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "core/json.h"
#include "tmini.h"

namespace {

struct Case {
  std::string text;
  bool ok;
  int line;
  int col;
  const char* why;
};

// Expected results produced with the JsonCheck class of firmware/webcheck/webcheck.ino itself
// (extracted and compiled on the host, 2026-11-08).
std::string webcheckNg() {
  return "{\n  \"_note\": \"webcheck の試験用ファイル（わざと壊してあります）\",\n  \"courses\": [\n    {\n"
         "      \"name\": \"袖ヶ浦フォレストレースウェイ\",\n"
         "      \"startFinish\": {\"lat\": 35.3949090, \"lon\": 140.0880580, \"heading\": 31.1 \"width\": 50},\n"
         "      \"splits\": [\n        {\"lat\": 35.3922588, \"lon\": 140.0909627, \"heading\": 250.1, \"width\": 30}\n"
         "      ]\n    }\n  ]\n}\n";
}

const Case* cases(int* n) {
  static const Case kCases[] = {
      {"{\"a\":1,}", false, 1, 8, "expected-key"},
      {"[1,]", false, 1, 4, "unexpected"},
      {"{\"a\" 1}", false, 1, 6, "expected-colon"},
      {"{\"a\":1 \"b\":2}", false, 1, 8, "expected-comma-or-brace"},
      {"[1 2]", false, 1, 4, "expected-comma-or-bracket"},
      {"{\"a\":1}x", false, 1, 8, "extra"},
      {"", false, 1, 1, "eof"},
      {"{\"a\":", false, 1, 6, "eof"},
      {"\"abc", false, 1, 5, "eof-in-string"},
      {std::string("\"a\x01\""), false, 1, 3, "control-in-string"},
      {"\"\\x\"", false, 1, 3, "bad-escape"},
      {"\"\\u12G4\"", false, 1, 6, "bad-escape"},
      {"-", false, 1, 2, "bad-number"},
      {"1.", false, 1, 3, "bad-number"},
      {"1e+", false, 1, 4, "bad-number"},
      {"tru", false, 1, 4, "eof"},
      {"trUe", false, 1, 3, "unexpected"},
      {std::string("\xEF\xBB\x00{}", 5), false, 1, 3, "bad-bom"},
      {"\xEF\xBB\xBF{}", true, 0, 0, ""},
      {"@", false, 1, 1, "unexpected"},
      {"{\"name\":\"袖ヶ浦\" \"x\":1}", false, 1, 15, "expected-comma-or-brace"},  // columns in characters
      {"{\r\n  \"a\": 1\r\n  \"b\": 2\r\n}", false, 3, 3, "expected-comma-or-brace"},
      {std::string(33, '['), false, 1, 33, "too-deep"},
      {std::string(32, '[') + std::string(32, ']'), true, 0, 0, ""},
      {webcheckNg(), false, 6, 78, "expected-comma-or-brace"},
      {"01", false, 1, 2, "extra"},
      {"{\"a\":[1,{\"b\":nul}]}", false, 1, 17, "unexpected"},
      {" \t\r\n 7 \n", true, 0, 0, ""},
      {"\"\\", false, 1, 3, "bad-escape"},
      {"{\"a\"", false, 1, 5, "eof"},
      {"[\"x\" , ]", false, 1, 8, "unexpected"},
  };
  *n = static_cast<int>(sizeof kCases / sizeof kCases[0]);
  return kCases;
}

bool sameError(const lt::json::Error& e, const Case& c) {
  return e.line == c.line && e.col == c.col && std::strcmp(e.why, c.why) == 0;
}

std::string show(const std::string& s) {
  std::string o;
  for (const unsigned char ch : s.substr(0, 40)) {
    if (ch >= 0x20 && ch < 0x7F) {
      o += static_cast<char>(ch);
    } else {
      char b[8];
      std::snprintf(b, sizeof b, "\\x%02X", ch);
      o += b;
    }
  }
  return o;
}

class FailingSource : public lt::json::Source {  // returns `good` bytes, then a read error
 public:
  FailingSource(const char* t, int good) : t_(t), good_(good) {}
  int read(uint8_t* buf, int cap) override {
    if (pos_ >= good_) return -1;
    int k = good_ - pos_;
    if (k > cap) k = cap;
    std::memcpy(buf, t_ + pos_, static_cast<size_t>(k));
    pos_ += k;
    return k;
  }

 private:
  const char* t_;
  int good_;
  int pos_ = 0;
};

uint64_t g_rng = 88172645463325252ull;
uint32_t rnd() {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return static_cast<uint32_t>(g_rng);
}

}  // namespace

TEST(json_check_reasons_positions_like_webcheck) {
  int n = 0;
  const Case* c = cases(&n);
  for (int i = 0; i < n; ++i) {
    for (int chunk : {0, 1, 3}) {  // whole, byte by byte, in small pieces
      lt::json::MemSource src(c[i].text.data(), static_cast<int>(c[i].text.size()), chunk);
      lt::json::Error e{};
      const bool ok = lt::json::check(src, &e);
      CHECKF(ok == c[i].ok && sameError(e, c[i]), "[%s] chunk %d: got %d %d:%d %s, expected %d %d:%d %s",
             show(c[i].text).c_str(), chunk, ok, e.line, e.col, e.why, c[i].ok, c[i].line, c[i].col, c[i].why);
    }
  }
  tmini::info("%d inputs, every reason code; expected values from webcheck.ino JsonCheck itself", n);
}

TEST(json_reader_finish_same_errors_as_check) {
  int n = 0;
  const Case* c = cases(&n);
  for (int i = 0; i < n; ++i) {
    lt::json::MemSource src(c[i].text.data(), static_cast<int>(c[i].text.size()));
    lt::json::Reader r(src);
    const bool ok = r.finish();
    CHECKF(ok == c[i].ok && sameError(r.error(), c[i]), "[%s]: reader %d %d:%d %s", show(c[i].text).c_str(), ok,
           r.error().line, r.error().col, r.error().why);
  }
  // random fragments: the reader (skipping everything) and check() agree on every input
  static const char* const kFrag[] = {"{", "}", "[", "]", ",", ":", "\"a\"", "1", "-", "0", "01", "1.", "1.5",
                                      "1e+5", "true", "nul", " ", "\n", "\"\\u00e9\"", "\"\\uZZ\"", "\"abc",
                                      "\xef\xbb\xbf", "\x01", "x", "\"\xe8\xa2\x96\""};
  const int numFrag = static_cast<int>(sizeof kFrag / sizeof kFrag[0]);
  int mismatches = 0;
  int valid = 0;
  for (int it = 0; it < 20000; ++it) {
    std::string s;
    const int nf = 1 + static_cast<int>(rnd() % 12);
    for (int k = 0; k < nf; ++k) s += kFrag[rnd() % static_cast<uint32_t>(numFrag)];
    lt::json::MemSource a(s.data(), static_cast<int>(s.size()));
    lt::json::Error e{};
    const bool ok1 = lt::json::check(a, &e);
    lt::json::MemSource b(s.data(), static_cast<int>(s.size()));
    lt::json::Reader r(b);
    const bool ok2 = r.finish();
    valid += ok1 ? 1 : 0;
    if (ok1 != ok2 || (!ok1 && (e.line != r.error().line || e.col != r.error().col ||
                                std::strcmp(e.why, r.error().why) != 0))) {
      if (++mismatches < 5) tmini::info("mismatch [%s]", show(s).c_str());
    }
  }
  CHECKF(mismatches == 0, "%d mismatches", mismatches);
  tmini::info("20000 random inputs (%d valid): reader and check() agree", valid);
}

TEST(json_check_read_error) {
  FailingSource src("{\"a\": [1, 2", 9);
  lt::json::Error e{};
  CHECK(!lt::json::check(src, &e));
  CHECKF(std::strcmp(e.why, "read-failed") == 0, "why %s", e.why);
  FailingSource src2("[1]", 3);  // the error comes after the value
  CHECK(!lt::json::check(src2, &e));
  CHECKF(std::strcmp(e.why, "read-failed") == 0, "why %s", e.why);
}

TEST(json_reader_values_and_escapes) {
  const char* text =
      "\xEF\xBB\xBF{\"s\":\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t\",\"u\":\"\\u00e9\\u30F6\",\"pair\":\"\\ud83d\\ude00\","
      "\"lone\":\"x\\ud800y\\udc00z\",\"raw\":\"袖ヶ浦\",\"t\":true,\"f\":false,\"n\":null,"
      "\"arr\":[1,-2.5,3e2],\"obj\":{\"k\":\"v\"},\"last\":7}";
  lt::json::MemSource src(text);
  lt::json::Reader r(src);
  CHECK(r.beginObject());
  char buf[64];
  int seen = 0;
  while (r.nextKey()) {
    ++seen;
    if (r.keyIs("s")) {
      CHECK(r.readString(buf, sizeof buf) == 12);
      CHECK(std::strcmp(buf, "a\"b\\c/d\b\f\n\r\t") == 0);
    } else if (r.keyIs("u")) {
      CHECK(r.readString(buf, sizeof buf) == 5);
      CHECK(std::strcmp(buf, "\xC3\xA9\xE3\x83\xB6") == 0);
    } else if (r.keyIs("pair")) {
      CHECK(r.readString(buf, sizeof buf) == 4);
      CHECK(std::strcmp(buf, "\xF0\x9F\x98\x80") == 0);
    } else if (r.keyIs("lone")) {  // a lone surrogate half becomes U+FFFD
      CHECK(r.readString(buf, sizeof buf) == 9);
      CHECK(std::strcmp(buf, "x\xEF\xBF\xBDy\xEF\xBF\xBDz") == 0);
    } else if (r.keyIs("raw")) {
      CHECK(r.readString(buf, sizeof buf) == 9);
      CHECK(std::strcmp(buf, "袖ヶ浦") == 0);
    } else if (r.keyIs("t") || r.keyIs("f")) {
      bool b = !r.keyIs("t");
      CHECK(r.readBool(&b));
      CHECK(b == r.keyIs("t"));
    } else if (r.keyIs("n")) {
      CHECK(r.peek() == lt::json::Type::Null);
      CHECK(r.skip());
    } else if (r.keyIs("arr")) {
      CHECK(r.peek() == lt::json::Type::Array);
      CHECK(r.beginArray());
      float v[3] = {0, 0, 0};
      int k = 0;
      while (r.nextItem()) {
        if (k < 3) CHECK(r.readFloat(&v[k]));
        ++k;
      }
      CHECK(k == 3 && v[0] == 1.0f && v[1] == -2.5f && v[2] == 300.0f);
    } else if (r.keyIs("obj")) {
      CHECK(r.beginObject());
      CHECK(r.nextKey() && r.keyIs("k"));
      CHECK(r.readString(buf, sizeof buf) == 1);
      CHECK(!r.nextKey());
    } else if (r.keyIs("last")) {
      int32_t x = 0;
      CHECK(r.readInt(&x) && x == 7);
    }
  }
  CHECKF(r.ok(), "%s at %d:%d", r.error().why, r.error().line, r.error().col);
  CHECK(seen == 11);
  CHECK(r.finish());
}

TEST(json_reader_string_cut_at_character) {
  const char* text = "[\"袖ヶ浦\",\"袖ヶ浦\",\"ab\\u00e9\",\"abc\"]";
  lt::json::MemSource src(text);
  lt::json::Reader r(src);
  char b[16];
  CHECK(r.beginArray());
  CHECK(r.nextItem());
  CHECK(r.readString(b, 5) == 9);  // 4 bytes of room: one 3-byte character
  CHECK(std::strcmp(b, "袖") == 0);
  CHECK(r.nextItem());
  CHECK(r.readString(b, 7) == 9);  // 6 bytes: two characters
  CHECK(std::strcmp(b, "袖ヶ") == 0);
  CHECK(r.nextItem());
  CHECK(r.readString(b, 4) == 4);  // "ab" + 2-byte e acute does not fit in 3
  CHECK(std::strcmp(b, "ab") == 0);
  CHECK(r.nextItem());
  CHECK(r.readString(nullptr, 0) == 3);  // measure only
  CHECK(!r.nextItem());
  CHECK(r.finish());
}

TEST(json_reader_numbers) {
  const char* text =
      "[35.3949090, 140.0880580, -0.00000005, 5000, 5000.5, 1e3, 3000000000, 1762567800000, 31.1, -0.5E-3,"
      " 1.005, 12345678901234567890123, 0.1234567890123456789012345, 1e400, 99999999999999999999]";
  lt::json::MemSource src(text);
  lt::json::Reader r(src);
  CHECK(r.beginArray());
  int32_t e7 = 0;
  int32_t i = 0;
  int64_t i64 = 0;
  float f = 0.0f;
  CHECK(r.nextItem() && r.readE7(&e7) && e7 == 353949090);
  CHECK(r.nextItem() && r.readE7(&e7) && e7 == 1400880580);
  CHECK(r.nextItem() && r.readE7(&e7) && e7 == -1);  // -0.5e-7 rounds away from zero
  CHECK(r.nextItem() && r.readInt(&i) && i == 5000);
  {
    lt::json::MemSource s2("5000.5");
    lt::json::Reader r2(s2);
    CHECK(!r2.readInt(&i) && std::strcmp(r2.error().why, "not-integer") == 0);
    CHECK(r.nextItem() && r.skip());
  }
  CHECK(r.nextItem() && r.readInt(&i) && i == 1000);
  {
    lt::json::MemSource s2("3000000000");
    lt::json::Reader r2(s2);
    CHECK(!r2.readInt(&i) && std::strcmp(r2.error().why, "number-range") == 0);
    CHECK(r.nextItem() && r.readInt64(&i64) && i64 == 3000000000ll);
  }
  CHECK(r.nextItem() && r.readInt64(&i64) && i64 == 1762567800000ll);
  CHECK(r.nextItem() && r.readFloat(&f) && f == 31.1f);
  CHECK(r.nextItem() && r.readFloat(&f) && f == -0.0005f);
  CHECK(r.nextItem() && r.readScaled(&i64, 2) && i64 == 101);  // exact decimal: 1.005 -> 1.01
  CHECK(r.nextItem() && r.readFloat(&f) && std::fabs(f / 1.2345678901234568e22f - 1.0f) < 1e-6f);
  CHECK(r.nextItem() && r.readScaled(&i64, 18) && i64 == 123456789012345679ll);
  CHECK(r.nextItem() && !r.readFloat(&f) && std::strcmp(r.error().why, "number-range") == 0);
  // reading continues to fail after the first error
  CHECK(!r.nextItem());
  // too long for the reader (checked numbers can be longer: check() accepts it)
  const std::string longNum = "1." + std::string(60, '5');
  lt::json::MemSource s3(longNum.c_str());
  lt::json::Reader r3(s3);
  CHECK(!r3.readFloat(&f) && std::strcmp(r3.error().why, "number-too-long") == 0);
  lt::json::MemSource s4(longNum.c_str());
  CHECK(lt::json::check(s4, nullptr));
}

TEST(json_reader_type_errors_and_positions) {
  {
    lt::json::MemSource src("{\"a\": \"x\"}");
    lt::json::Reader r(src);
    float f = 0.0f;
    CHECK(r.beginObject() && r.nextKey());
    CHECK(!r.readFloat(&f));
    CHECKF(std::strcmp(r.error().why, "expected-number") == 0 && r.error().line == 1 && r.error().col == 7,
           "%s %d:%d", r.error().why, r.error().line, r.error().col);
  }
  {
    lt::json::MemSource src("\n  [1]");
    lt::json::Reader r(src);
    CHECK(!r.beginObject());
    CHECKF(std::strcmp(r.error().why, "expected-object") == 0 && r.error().line == 2 && r.error().col == 3, "%s %d:%d",
           r.error().why, r.error().line, r.error().col);
  }
  {
    lt::json::MemSource src("{\"a\": 1}");
    lt::json::Reader r(src);
    bool b = false;
    CHECK(r.beginObject() && r.nextKey() && !r.readBool(&b) && std::strcmp(r.error().why, "expected-bool") == 0);
  }
  {  // fail(): content error at the value
    lt::json::MemSource src("{\n \"version\": 2}");
    lt::json::Reader r(src);
    int32_t v = 0;
    CHECK(r.beginObject() && r.nextKey() && r.readInt(&v) && v == 2);
    CHECK(!r.fail("bad-version"));
    CHECKF(std::strcmp(r.error().why, "bad-version") == 0 && r.error().line == 2 && r.error().col == 13, "%s %d:%d",
           r.error().why, r.error().line, r.error().col);
  }
  {  // a value read without nextKey()
    lt::json::MemSource src("{\"a\": 1}");
    lt::json::Reader r(src);
    int32_t v = 0;
    CHECK(r.beginObject() && !r.readInt(&v) && std::strcmp(r.error().why, "no-value") == 0);
  }
}

TEST(json_reader_skips_what_is_not_read) {
  const char* text = "{\"skipme\": {\"deep\": [1, [2, {\"x\": \"]}\"}]]}, \"want\": 5, \"tail\": [true, null]}";
  lt::json::MemSource src(text);
  lt::json::Reader r(src);
  int32_t want = 0;
  CHECK(r.beginObject());
  while (r.nextKey()) {
    if (r.keyIs("want")) CHECK(r.readInt(&want));
  }
  CHECK(want == 5);
  CHECK(r.finish());
  // stopping early: finish() checks and skips the rest
  lt::json::MemSource src2(text);
  lt::json::Reader r2(src2);
  CHECK(r2.beginObject() && r2.nextKey() && r2.beginObject());
  CHECK(r2.finish());
  lt::json::MemSource src3("{\"a\": [1, 2}");
  lt::json::Reader r3(src3);
  CHECK(r3.beginObject() && r3.nextKey());
  CHECK(!r3.finish() && std::strcmp(r3.error().why, "expected-comma-or-bracket") == 0);
}

TEST(json_writer_output_and_overflow) {
  char buf[512];
  lt::json::Writer w(buf, sizeof buf);
  w.beginObject().key("version").i64(1).key("name").str("袖ヶ浦 \"F\"\\\n\x01").key("list").beginArray();
  w.beginObject(true).key("lat").scaled(353949090, 7).key("h").num(31.1f, 2).key("w").num(50.0f, 1);
  w.key("neg").num(-0.0004f, 3).key("t").boolean(true).key("n").null().key("e").beginArray().endArray().endObject();
  w.endArray().key("empty").beginObject().endObject().endObject();
  const int n = w.finish();
  const char* expect =
      "{\n"
      "  \"version\": 1,\n"
      "  \"name\": \"袖ヶ浦 \\\"F\\\"\\\\\\n\\u0001\",\n"
      "  \"list\": [\n"
      "    {\"lat\": 35.3949090, \"h\": 31.1, \"w\": 50, \"neg\": 0, \"t\": true, \"n\": null, \"e\": []}\n"
      "  ],\n"
      "  \"empty\": {}\n"
      "}\n";
  CHECKF(n == static_cast<int>(std::strlen(expect)) && std::strcmp(buf, expect) == 0, "got:\n%s", buf);
  // compact
  char c[64];
  lt::json::Writer wc(c, sizeof c, false);
  wc.beginObject().key("a").beginArray().i64(-5).scaled(-5, 3).endArray().key("b").str("").endObject();
  CHECKF(wc.finish() > 0 && std::strcmp(c, "{\"a\":[-5,-0.005],\"b\":\"\"}") == 0, "got %s", c);
  // every buffer smaller than the text gives 0, one byte more than the text is enough
  int bad = 0;
  for (int cap = 0; cap <= n + 1; ++cap) {
    char small[600];
    lt::json::Writer ws(small, cap);
    ws.beginObject().key("version").i64(1).key("name").str("袖ヶ浦 \"F\"\\\n\x01").key("list").beginArray();
    ws.beginObject(true).key("lat").scaled(353949090, 7).key("h").num(31.1f, 2).key("w").num(50.0f, 1);
    ws.key("neg").num(-0.0004f, 3).key("t").boolean(true).key("n").null().key("e").beginArray().endArray().endObject();
    ws.endArray().key("empty").beginObject().endObject().endObject();
    const int m = ws.finish();
    if ((cap <= n && m != 0) || (cap == n + 1 && m != n)) ++bad;
  }
  CHECKF(bad == 0, "%d capacities wrong", bad);
  // misuse: value without a key, unbalanced
  char m[64];
  lt::json::Writer w1(m, sizeof m);
  w1.beginObject().i64(1).endObject();
  CHECK(w1.finish() == 0);
  lt::json::Writer w2(m, sizeof m);
  w2.beginArray();
  CHECK(w2.finish() == 0);
}

TEST(json_decimal_conversions) {
  char t[40];
  CHECK(lt::json::formatScaled(t, sizeof t, 353949090, 7) == 10 && std::strcmp(t, "35.3949090") == 0);
  CHECK(lt::json::formatScaled(t, sizeof t, -5, 3) == 6 && std::strcmp(t, "-0.005") == 0);
  CHECK(lt::json::formatScaled(t, sizeof t, 0, 2) == 4 && std::strcmp(t, "0.00") == 0);
  CHECK(lt::json::formatScaled(t, sizeof t, INT64_MIN, 0) == 20 && std::strcmp(t, "-9223372036854775808") == 0);
  CHECK(lt::json::formatScaled(t, 4, 12345, 0) == 0);  // too small
  CHECK(lt::json::formatFloat(t, sizeof t, 176.4f, 1) > 0 && std::strcmp(t, "176.4") == 0);
  CHECK(lt::json::formatFloat(t, sizeof t, -12.345f, 2) > 0 && std::strcmp(t, "-12.35") == 0);
  CHECK(lt::json::formatFloat(t, sizeof t, -0.0004f, 3) > 0 && std::strcmp(t, "0.000") == 0);
  CHECK(lt::json::formatFloat(t, sizeof t, NAN, 2) == 0);
  CHECK(lt::json::formatFloat(t, sizeof t, INFINITY, 2) == 0);
  int64_t v = 0;
  bool exact = false;
  CHECK(lt::json::parseScaled("+2123.694540", 12, 8, &v, &exact) && v == 212369454000ll && exact);
  CHECK(lt::json::parseScaled("-08405.33340", 12, 5, &v, &exact) && v == -840533340ll && exact);
  CHECK(lt::json::parseScaled("0.125", 5, 2, &v, &exact) && v == 13 && !exact);
  CHECK(lt::json::parseScaled("-0.125", 6, 2, &v, &exact) && v == -13 && !exact);
  CHECK(lt::json::parseScaled("9223372036854775807", 19, 0, &v) && v == INT64_MAX);
  CHECK(!lt::json::parseScaled("9223372036854775808", 19, 0, &v));
  CHECK(!lt::json::parseScaled("1.2.3", 5, 0, &v));
  CHECK(!lt::json::parseScaled("", 0, 0, &v));
  CHECK(!lt::json::parseScaled("e5", 2, 0, &v));
  float f = 0.0f;
  CHECK(lt::json::parseFloat("252.1", 5, &f) && f == 252.1f);
  CHECK(lt::json::parseFloat("-1.5e-2", 7, &f) && f == -0.015f);
  CHECK(!lt::json::parseFloat("1e39", 4, &f));
  // every E7 value survives text and back
  int bad = 0;
  for (int k = 0; k < 20000; ++k) {
    const int32_t e7 = static_cast<int32_t>(rnd());
    const int n = lt::json::formatScaled(t, sizeof t, e7, 7);
    int64_t back = 0;
    if (!lt::json::parseScaled(t, n, 7, &back) || back != e7) ++bad;
  }
  CHECKF(bad == 0, "%d E7 values changed", bad);
}
