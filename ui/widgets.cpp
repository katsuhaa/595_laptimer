// ui/widgets.cpp - text helpers, slanted tags, pills, badges, page header, formatting (shared by the screens)
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_int.h"

namespace ui {
namespace in {

uint16_t dim35(uint16_t c) {
  return gfx::blend(C_BG, c, 89);
}
uint16_t statusColor(uint8_t st) {
  switch (st) {
    case ST_OK: return C_GREEN;
    case ST_WARN: return C_YELLOW;
    case ST_ERR: return C_RED;
    default: return C_DOT;
  }
}

// ---------------------------------------------------------------- text
const TextSet T13 = { &FONT_CP13, &FONT_JP13 };
const TextSet T14 = { &FONT_CP14, &FONT_JP14 };
const TextSet T18 = { &FONT_CP18, &FONT_JP18 };
const TextSet T22 = { &FONT_CP22, &FONT_JP22 };
const TextSet T26 = { &FONT_CP26, &FONT_JP26 };
const TextSet T28 = { &FONT_CP28, &FONT_JP28 };
const TextSet T30 = { &FONT_CP30, &FONT_JP30 };

static inline const FontData &pick(const TextSet &t, uint32_t code) {
  return gfx::hasGlyph(*t.lat, code) ? *t.lat : *t.jp;
}

float mixWidth(const TextSet &t, const char *s) {
  float w = 0;
  for (uint32_t code; (code = gfx::nextCode(s)) != 0;) w += gfx::glyphAdvance(pick(t, code), code);
  return w;
}
float mixDraw(const TextSet &t, float x, float base, const char *s, uint16_t c) {
  for (uint32_t code; (code = gfx::nextCode(s)) != 0;) x = gfx::drawGlyph(pick(t, code), x, base, code, c);
  return x;
}
float mixDrawR(const TextSet &t, float xr, float base, const char *s, uint16_t c) {
  return mixDraw(t, xr - mixWidth(t, s), base, s, c);
}
float mixDrawC(const TextSet &t, float cx, float base, const char *s, uint16_t c) {
  return mixDraw(t, cx - mixWidth(t, s) * 0.5f, base, s, c);
}

float mixDrawFit(const TextSet *sets, int nSets, float x, float base, float maxW, const char *s, uint16_t c) {
  for (int i = 0; i < nSets; i++)
    if (mixWidth(sets[i], s) <= maxW) return mixDraw(sets[i], x, base, s, c);
  const TextSet &t = sets[nSets - 1];
  const char *ell = "…";
  const float we = mixWidth(t, ell);
  char buf[96];
  size_t used = 0;
  float w = 0;
  const char *p = s;
  for (;;) {
    const char *q = p;
    uint32_t code = gfx::nextCode(q);
    if (!code) break;
    float a = gfx::glyphAdvance(pick(t, code), code);
    size_t len = (size_t)(q - p);
    if (w + a + we > maxW || used + len + 4 >= sizeof buf) break;
    memcpy(buf + used, p, len);
    used += len;
    w += a;
    p = q;
  }
  memcpy(buf + used, ell, strlen(ell) + 1);
  return mixDraw(t, x, base, buf, c);
}

float textC(const FontData &f, float cx, float base, const char *s, uint16_t c) {
  return gfx::drawText(f, cx - gfx::textWidth(f, s) * 0.5f, base, s, c);
}
float textR(const FontData &f, float xr, float base, const char *s, uint16_t c) {
  return gfx::drawText(f, xr - gfx::textWidth(f, s), base, s, c);
}
float labelW(const FontData &f, const char *s, float em) {
  gfx::setTracking(em * (float)f.px);
  float w = gfx::textWidth(f, s);
  gfx::setTracking(0);
  return w;
}
float label(const FontData &f, float x, float base, const char *s, uint16_t c, float em) {
  gfx::setTracking(em * (float)f.px);
  float r = gfx::drawText(f, x, base, s, c);
  gfx::setTracking(0);
  return r;
}

// ---------------------------------------------------------------- shapes
void fillPara(float x, float y, float w, float h, float slant, uint16_t c) {
  gfx::Pt p[4] = { { x, y + h }, { x + w, y + h }, { x + w + slant, y }, { x + slant, y } };
  gfx::fillPoly(p, 4, c);
}
void framePara(float x, float y, float w, float h, float slant, float border, uint16_t cBorder, uint16_t cFill) {
  fillPara(x, y, w, h, slant, cBorder);
  const float d = border * sqrtf(h * h + slant * slant) / h;  // horizontal inset of the slanted sides
  const float ih = h - 2 * border;
  if (ih <= 0 || w - 2 * d <= 0) return;
  fillPara(x + slant * border / h + d, y + border, w - 2 * d, ih, slant * ih / h, cFill);
}

void checkMark(float cx, float cy, float s, uint16_t c) {
  const float w = s * 0.17f;
  gfx::Pt a = { cx - s * 0.36f, cy + s * 0.02f }, b = { cx - s * 0.10f, cy + s * 0.28f }, e = { cx + s * 0.38f, cy - s * 0.26f };
  auto quad = [&](gfx::Pt p0, gfx::Pt p1, gfx::Pt *q) {
    float dx = p1.x - p0.x, dy = p1.y - p0.y, l = sqrtf(dx * dx + dy * dy);
    float nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
    float ex = dx / l * w * 0.5f, ey = dy / l * w * 0.5f;  // extend the ends so that the joint is closed
    q[0] = { p0.x + nx - ex, p0.y + ny - ey };
    q[1] = { p1.x + nx + ex, p1.y + ny + ey };
    q[2] = { p1.x - nx + ex, p1.y - ny + ey };
    q[3] = { p0.x - nx - ex, p0.y - ny - ey };
  };
  gfx::Pt q1[4], q2[4];
  quad(a, b, q1);
  quad(b, e, q2);
  gfx::Pt all[8];
  memcpy(all, q1, sizeof q1);
  memcpy(all + 4, q2, sizeof q2);
  gfx::polyBegin(gfx::ptsRect(all, 8));
  gfx::polyAdd(q1, 4);
  gfx::polyAdd(q2, 4);
  gfx::polyFill(c);
}
void crossMark(float cx, float cy, float s, uint16_t c) {
  const float h = s * 0.32f, w = s * 0.17f;
  gfx::Pt q1[4], q2[4];
  auto quad = [&](float x0, float y0, float x1, float y1, gfx::Pt *q) {
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    float nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
    q[0] = { x0 + nx, y0 + ny };
    q[1] = { x1 + nx, y1 + ny };
    q[2] = { x1 - nx, y1 - ny };
    q[3] = { x0 - nx, y0 - ny };
  };
  quad(cx - h, cy - h, cx + h, cy + h, q1);
  quad(cx - h, cy + h, cx + h, cy - h, q2);
  gfx::Pt all[8];
  memcpy(all, q1, sizeof q1);
  memcpy(all + 4, q2, sizeof q2);
  gfx::polyBegin(gfx::ptsRect(all, 8));
  gfx::polyAdd(q1, 4);
  gfx::polyAdd(q2, 4);
  gfx::polyFill(c);
}

// triangle tip, base centre +- n * bw/2; grown by e around its incentre
static void triPts(gfx::Pt tip, gfx::Pt baseC, float bw, float e, gfx::Pt p[3]) {
  float dx = tip.x - baseC.x, dy = tip.y - baseC.y, l = sqrtf(dx * dx + dy * dy);
  float nx = -dy / l, ny = dx / l;
  gfx::Pt A = tip, B = { baseC.x + nx * bw * 0.5f, baseC.y + ny * bw * 0.5f }, C = { baseC.x - nx * bw * 0.5f, baseC.y - ny * bw * 0.5f };
  if (e == 0) {
    p[0] = A;
    p[1] = B;
    p[2] = C;
    return;
  }
  auto dist = [](gfx::Pt u, gfx::Pt v) { return sqrtf((u.x - v.x) * (u.x - v.x) + (u.y - v.y) * (u.y - v.y)); };
  float a = dist(B, C), b = dist(A, C), c = dist(A, B), per = a + b + c;
  gfx::Pt I = { (a * A.x + b * B.x + c * C.x) / per, (a * A.y + b * B.y + c * C.y) / per };
  float area = fabsf((B.x - A.x) * (C.y - A.y) - (C.x - A.x) * (B.y - A.y)) * 0.5f;
  float r = 2 * area / per, k = (r + e) / r;
  gfx::Pt V[3] = { A, B, C };
  for (int i = 0; i < 3; i++) p[i] = { I.x + (V[i].x - I.x) * k, I.y + (V[i].y - I.y) * k };
}
void edgedTriangle(gfx::Pt tip, gfx::Pt baseC, float bw, float e, uint16_t fill, uint16_t edge) {
  gfx::Pt p[3];
  triPts(tip, baseC, bw, e, p);
  gfx::fillPoly(p, 3, edge);
  triPts(tip, baseC, bw, 0, p);
  gfx::fillPoly(p, 3, fill);
}
gfx::Rect triangleRect(gfx::Pt tip, gfx::Pt baseC, float bw, float e) {
  gfx::Pt p[3];
  triPts(tip, baseC, bw, e, p);
  return gfx::ptsRect(p, 3);
}

void pill(float x, float y, float w, float h, const char *text, uint16_t dot, uint16_t fg) {
  gfx::fillRoundRect(x, y, w, h, h * 0.5f, C_PANEL);
  gfx::fillCircle(x + h * 0.5f + 1.0f, y + h * 0.5f, 3.2f, dot);
  const float tx = x + h * 0.5f + 7.0f;
  mixDraw(T13, tx, y + h * 0.5f + (float)FONT_CP13.capH * 0.5f, text, fg);
}

static float centreBase(const TextSet &t, const char *s, float cy) {
  bool ascii = true;
  for (const char *p = s; *p; p++)
    if ((uint8_t)*p >= 0x80) ascii = false;
  const FontData &f = ascii ? *t.lat : *t.jp;
  return cy + (float)f.capH * 0.5f;
}
void badge(float x, float y, float w, float h, float r, const TextSet &t, const char *s, uint16_t bg, uint16_t fg) {
  gfx::fillRoundRect(x, y, w, h, r, bg);
  mixDrawC(t, x + w * 0.5f, centreBase(t, s, y + h * 0.5f), s, fg);
}
void button(float x, float y, float w, float h, const TextSet &t, const char *s, uint16_t border, uint16_t fill, uint16_t fg) {
  gfx::frameRoundRect(x, y, w, h, 10, 3, border, fill);
  mixDrawC(t, x + w * 0.5f, centreBase(t, s, y + h * 0.5f), s, fg);
}

// ---------------------------------------------------------------- page header
static const PageStyle STYLES[] = {
  { C_YELLOW, C_TINT_YELLOW, &ICON_LAPS, &ICON_LAPS_2, "ラップ" },
  { C_ORANGE, C_TINT_ORANGE, &ICON_MAINT, &ICON_MAINT_2, "整備" },
  { C_BLUE, C_TINT_BLUE, &ICON_DATA, &ICON_DATA_2, "データ" },
  { C_SUB, C_TINT_GREY, &ICON_SET, &ICON_SET_2, "設定" },
};
const PageStyle &pageStyle(uint8_t screen) {
  int i = screen >= SC_LAPS && screen <= SC_SETTINGS ? screen - SC_LAPS : 3;
  return STYLES[i];
}
int pageIndex(uint8_t screen) {
  if (screen == SC_DRIVE) return 0;
  if (screen >= SC_LAPS && screen <= SC_SETTINGS) return 1 + (screen - SC_LAPS);
  return -1;
}

void drawHeader(uint8_t screen, const char *aux) {
  const PageStyle &st = pageStyle(screen);
  gfx::fillRect(0, 0, gfx::W, kHeaderH, st.tint);
  gfx::fillRect(0, kHeaderH, gfx::W, 2, st.color);
  gfx::Pt band[4] = { { 0, 0 }, { 18, 0 }, { 2, (float)kHeaderH }, { 0, (float)kHeaderH } };  // slanted colour band (10 px on average)
  gfx::fillPoly(band, 4, st.color);
  gfx::drawIcon(*st.icon2, 30, 14, gfx::blend(st.tint, st.color, 80));
  gfx::drawIcon(*st.icon, 30, 14, st.color);
  const float base = 32.0f + (float)FONT_JP30.capH * 0.5f;
  float x = mixDraw(T30, 78, base, st.name, st.color);
  // dots: one per page (drive, laps, maint, data, settings); the current one larger in the page colour
  const int cur = pageIndex(screen);
  const float dotX0 = 500;
  for (int i = 0; i < 5; i++) {
    const bool on = i == cur;
    gfx::fillCircle(dotX0 + 5.0f + 18.0f * (float)i + (i > cur ? 2.0f : 0.0f) + (on ? 1.0f : 0.0f), 32.0f, on ? 6.0f : 5.0f, on ? st.color : C_DOT);
  }
  if (aux && *aux) {
    const TextSet sets[1] = { T18 };
    mixDrawFit(sets, 1, x + 14, 32.0f + (float)FONT_JP18.capH * 0.5f, dotX0 - 12 - (x + 14), aux, C_LABEL);
  }
}

// ---------------------------------------------------------------- formatting
void fmtLap(char *o, size_t n, uint32_t ms) {
  uint32_t cs = ms / 10;
  if (cs > 99u * 6000u + 5999u) cs = 99u * 6000u + 5999u;
  snprintf(o, n, "%u:%02u.%02u", (unsigned)(cs / 6000u), (unsigned)(cs / 100u % 60u), (unsigned)(cs % 100u));
}
void fmtSec2(char *o, size_t n, uint32_t ms) {
  if (ms < 60000u) snprintf(o, n, "%u.%02u", (unsigned)(ms / 1000u), (unsigned)(ms / 10u % 100u));
  else fmtLap(o, n, ms);
}
void fmtDelta(char *o, size_t n, float s, bool plusMinus) {
  long cs = lroundf(fabsf(s) * 100.0f);
  if (cs > 9999) cs = 9999;
  const char *sign = s < 0 ? "−" : "+";  // U+2212
  if (!plusMinus) sign = s < 0 ? "-" : "+";
  snprintf(o, n, "%s%ld.%02ld", sign, cs / 100, cs % 100);
}
void fmtGrouped(char *o, size_t n, uint32_t v) {
  char t[16];
  snprintf(t, sizeof t, "%u", (unsigned)v);
  size_t len = strlen(t), j = 0;
  for (size_t i = 0; i < len && j + 2 < n; i++) {
    o[j++] = t[i];
    size_t left = len - 1 - i;
    if (left && left % 3 == 0) o[j++] = ',';
  }
  o[j < n ? j : n - 1] = 0;
}
void fmtTenths(char *o, size_t n, float v) {
  long t = lroundf(v * 10.0f);
  if (t < 0) t = 0;
  if (t > 9999) t = 9999;
  snprintf(o, n, "%ld.%ld", t / 10, t % 10);
}
void fmtDate(char *o, size_t n, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi) {
  snprintf(o, n, "%u/%u %02u:%02u", (unsigned)mo, (unsigned)d, (unsigned)h, (unsigned)mi);
}

// ---------------------------------------------------------------- small helpers
uint32_t hashBytes(uint32_t h, const void *p, size_t n) {
  const uint8_t *b = (const uint8_t *)p;
  for (size_t i = 0; i < n; i++) {
    h ^= b[i];
    h *= 16777619u;
  }
  return h;
}
uint32_t hashStr(uint32_t h, const char *s) {
  return hashBytes(h, s, strlen(s) + 1);
}
bool overlap(const gfx::Rect &a, const gfx::Rect &b) {
  return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}
gfx::Rect rectXY(int x0, int y0, int x1, int y1) {
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > gfx::W) x1 = gfx::W;
  if (y1 > gfx::H) y1 = gfx::H;
  if (x1 < x0) x1 = x0;
  if (y1 < y0) y1 = y0;
  return gfx::Rect{ (int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1 };
}
gfx::Rect unite(const gfx::Rect &a, const gfx::Rect &b) {
  if (emptyRect(a)) return b;
  if (emptyRect(b)) return a;
  return gfx::Rect{ a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1 };
}
bool emptyRect(const gfx::Rect &r) {
  return r.x1 <= r.x0 || r.y1 <= r.y0;
}
float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static inline int areaOf(const gfx::Rect &r) {
  return (int)(r.x1 - r.x0) * (int)(r.y1 - r.y0);
}
void RectList::add(const gfx::Rect &a0, int slack) {
  if (emptyRect(a0)) return;
  gfx::Rect a = a0;
  for (;;) {
    int hit = -1;
    for (int i = 0; i < n; i++) {
      gfx::Rect u = unite(r[i], a);
      if (areaOf(u) - areaOf(r[i]) - areaOf(a) <= slack) {
        hit = i;
        a = u;
        break;
      }
    }
    if (hit < 0) break;
    r[hit] = r[--n];
  }
  if (n == kMax) {  // full: grow the rect that grows least
    int best = 0, bestGrow = 0x7FFFFFFF;
    for (int i = 0; i < n; i++) {
      int grow = areaOf(unite(r[i], a)) - areaOf(r[i]);
      if (grow < bestGrow) {
        bestGrow = grow;
        best = i;
      }
    }
    r[best] = unite(r[best], a);
    return;
  }
  r[n++] = a;
}

}  // namespace in
}  // namespace ui
