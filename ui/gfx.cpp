// ui/gfx.cpp - drawing into the 600x450 RGB565 frame buffer. See gfx.h.
// Ported from abarth595-round-gauge meter_display/gfx.cpp; the algorithms are unchanged, every write now
// goes through the clip rectangle and the opacity multiplier.
#include "gfx.h"

#include <math.h>
#include <string.h>

namespace ui {
namespace gfx {

static uint16_t *g_fb = nullptr;      // current target
static uint16_t *g_screen = nullptr;  // frame buffer from begin()
static float *g_acc = nullptr;
static bool g_tabular = true;
static uint32_t g_missing = 0;
static float g_track = 0.0f;
static uint32_t g_alpha = 255;
static int g_cx0 = 0, g_cy0 = 0, g_cx1 = W, g_cy1 = H;  // clip

void begin(uint16_t *framebuffer, float *scratch) {
  g_fb = framebuffer;
  g_screen = framebuffer;
  g_acc = scratch;
  g_alpha = 255;
  g_track = 0.0f;
  clearClip();
  dirtyClear();
}
uint16_t *frame() {
  return g_fb;
}
uint16_t *screen() {
  return g_screen;
}
void setTarget(uint16_t *fb) {
  g_fb = fb ? fb : g_screen;
}

void setClip(int x0, int y0, int x1, int y1) {
  g_cx0 = x0 < 0 ? 0 : x0;
  g_cy0 = y0 < 0 ? 0 : y0;
  g_cx1 = x1 > W ? W : x1;
  g_cy1 = y1 > H ? H : y1;
  if (g_cx1 < g_cx0) g_cx1 = g_cx0;
  if (g_cy1 < g_cy0) g_cy1 = g_cy0;
}
void setClip(const Rect &r) {
  setClip(r.x0, r.y0, r.x1, r.y1);
}
void clearClip() {
  g_cx0 = 0;
  g_cy0 = 0;
  g_cx1 = W;
  g_cy1 = H;
}
Rect clip() {
  return Rect{ (int16_t)g_cx0, (int16_t)g_cy0, (int16_t)g_cx1, (int16_t)g_cy1 };
}
void setAlpha(uint8_t a) {
  g_alpha = a;
}
static inline int withAlpha(int a) {
  return g_alpha >= 255 ? a : (int)(((uint32_t)a * g_alpha + 127) / 255);
}

// ------------------------------------------------------------ dirty rectangles
static const int MAX_DIRTY = 48;
static const int MERGE_SLACK = 1500;  // merge two rects when the union adds at most this many pixels
static Rect g_dirty[MAX_DIRTY];
static int g_nDirty = 0;

static inline int rectArea(const Rect &r) {
  return (int)(r.x1 - r.x0) * (int)(r.y1 - r.y0);
}
static inline Rect rectUnion(const Rect &a, const Rect &b) {
  Rect u;
  u.x0 = a.x0 < b.x0 ? a.x0 : b.x0;
  u.y0 = a.y0 < b.y0 ? a.y0 : b.y0;
  u.x1 = a.x1 > b.x1 ? a.x1 : b.x1;
  u.y1 = a.y1 > b.y1 ? a.y1 : b.y1;
  return u;
}

void addDirty(int x0, int y0, int x1, int y1) {
  if (g_fb != g_screen) return;  // drawing into a layer
  if (x0 < g_cx0) x0 = g_cx0;
  if (y0 < g_cy0) y0 = g_cy0;
  if (x1 > g_cx1) x1 = g_cx1;
  if (y1 > g_cy1) y1 = g_cy1;
  if (x1 <= x0 || y1 <= y0) return;
  Rect r = { (int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1 };
  for (;;) {
    int hit = -1;
    for (int i = 0; i < g_nDirty; i++) {
      Rect u = rectUnion(g_dirty[i], r);
      if (rectArea(u) - rectArea(g_dirty[i]) - rectArea(r) <= MERGE_SLACK) {
        hit = i;
        r = u;
        break;
      }
    }
    if (hit < 0) break;
    g_dirty[hit] = g_dirty[--g_nDirty];  // compare the merged rect with the rest again
  }
  if (g_nDirty == MAX_DIRTY) {
    int best = 0, bestGrow = 0x7FFFFFFF;
    for (int i = 0; i < g_nDirty; i++) {
      int grow = rectArea(rectUnion(g_dirty[i], r)) - rectArea(g_dirty[i]);
      if (grow < bestGrow) {
        bestGrow = grow;
        best = i;
      }
    }
    g_dirty[best] = rectUnion(g_dirty[best], r);
    return;
  }
  g_dirty[g_nDirty++] = r;
}
int dirtyCount() {
  return g_nDirty;
}
Rect dirtyAt(int i) {
  return g_dirty[i];
}
void dirtyClear() {
  g_nDirty = 0;
}
uint32_t dirtyPixels() {
  uint32_t n = 0;
  for (int i = 0; i < g_nDirty; i++) n += (uint32_t)rectArea(g_dirty[i]);
  return n;
}

// ------------------------------------------------------------ pixels
static inline void fill16(uint16_t *p, int n, uint16_t c) {
  while (n-- > 0) *p++ = c;
}
// one row [xa, xb) with coverage a (0..255)
static inline void span(int xa, int xb, int y, uint16_t c, int a) {
  if (y < g_cy0 || y >= g_cy1) return;
  a = withAlpha(a);
  if (a <= 0) return;
  if (xa < g_cx0) xa = g_cx0;
  if (xb > g_cx1) xb = g_cx1;
  if (xb <= xa) return;
  uint16_t *p = g_fb + y * W + xa;
  if (a >= 255) {
    fill16(p, xb - xa, c);
  } else {
    for (int n = xb - xa; n > 0; n--, p++) *p = blend(*p, c, (uint32_t)a);
  }
}
static bool g_onBase = false;  // while true, edge pixels mix with g_base (the screen is not read)
static uint16_t g_base = 0;
static inline void plot(int x, int y, uint16_t c, float cov) {
  if (x < g_cx0 || x >= g_cx1 || y < g_cy0 || y >= g_cy1) return;
  int a = withAlpha((int)(cov * 255.0f + 0.5f));
  if (a <= 0) return;
  uint16_t *p = g_fb + y * W + x;
  *p = (a >= 255) ? c : blend(g_onBase ? g_base : *p, c, (uint32_t)a);
}

void fillAll(uint16_t c) {
  for (int y = g_cy0; y < g_cy1; y++) span(g_cx0, g_cx1, y, c, 255);
  addDirty(0, 0, W, H);
}

void fillRect(int x, int y, int w, int h, uint16_t c) {
  int y0 = y < g_cy0 ? g_cy0 : y, y1 = y + h > g_cy1 ? g_cy1 : y + h;
  for (int iy = y0; iy < y1; iy++) span(x, x + w, iy, c, 255);
  addDirty(x, y, x + w, y + h);
}

void copyRectFrom(const uint16_t *src, int x0, int y0, int x1, int y1) {
  if (x0 < g_cx0) x0 = g_cx0;
  if (y0 < g_cy0) y0 = g_cy0;
  if (x1 > g_cx1) x1 = g_cx1;
  if (y1 > g_cy1) y1 = g_cy1;
  if (x1 <= x0 || y1 <= y0 || src == g_fb) return;
  for (int y = y0; y < y1; y++) memcpy(g_fb + y * W + x0, src + y * W + x0, (size_t)(x1 - x0) * sizeof(uint16_t));
  addDirty(x0, y0, x1, y1);
}

void copyDiscFrom(const uint16_t *src, int x0, int y0, int x1, int y1, float cx, float cy, float r) {
  if (x0 < g_cx0) x0 = g_cx0;
  if (y0 < g_cy0) y0 = g_cy0;
  if (x1 > g_cx1) x1 = g_cx1;
  if (y1 > g_cy1) y1 = g_cy1;
  if (x1 <= x0 || y1 <= y0 || src == g_fb) return;
  const float r2 = r * r;
  for (int y = y0; y < y1; y++) {
    const float dy = (float)y + 0.5f - cy;
    const float q = r2 - dy * dy;
    if (q < 0) continue;
    const float hw = sqrtf(q);
    int xa = (int)ceilf(cx - hw - 0.5f), xb = (int)floorf(cx + hw - 0.5f) + 1;  // pixel centre inside
    if (xa < x0) xa = x0;
    if (xb > x1) xb = x1;
    if (xb > xa) memcpy(g_fb + y * W + xa, src + y * W + xa, (size_t)(xb - xa) * sizeof(uint16_t));
  }
  addDirty(x0, y0, x1, y1);
}

void fillRectF(float x0, float y0, float x1, float y1, uint16_t c) {
  if (x1 <= x0 || y1 <= y0) return;
  int ix0 = (int)floorf(x0), ix1 = (int)ceilf(x1);
  int iy0 = (int)floorf(y0), iy1 = (int)ceilf(y1);
  for (int iy = iy0; iy < iy1; iy++) {
    if (iy < g_cy0 || iy >= g_cy1) continue;
    float vy = fminf(y1, (float)(iy + 1)) - fmaxf(y0, (float)iy);
    if (vy <= 0) continue;
    int fullA = ix0, fullB = ix1;
    float vxl = fminf(x1, (float)(ix0 + 1)) - x0;
    if (vxl < 0.999f) {
      plot(ix0, iy, c, vxl * vy);
      fullA = ix0 + 1;
    }
    if (ix1 - 1 >= fullA) {
      float vxr = x1 - (float)(ix1 - 1);
      if (vxr < 0.999f) {
        plot(ix1 - 1, iy, c, vxr * vy);
        fullB = ix1 - 1;
      }
    }
    span(fullA, fullB, iy, c, (int)(vy * 255.0f + 0.5f));
  }
  addDirty(ix0, iy0, ix1, iy1);
}

// signed distance to a round rect (inside negative); every corner can have its own radius
struct RRect {
  float cx, cy, hw, hh, rTL, rTR, rBR, rBL;
};
static inline float sdRRect(const RRect &q, float px, float py) {
  float dx = px - q.cx, dy = py - q.cy;
  float r = (dx < 0) ? (dy < 0 ? q.rTL : q.rBL) : (dy < 0 ? q.rTR : q.rBR);
  float ax = fabsf(dx) - q.hw + r, ay = fabsf(dy) - q.hh + r;
  float mx = ax > 0 ? ax : 0, my = ay > 0 ? ay : 0;
  float inside = (ax > ay ? ax : ay);
  if (inside > 0) inside = 0;
  return sqrtf(mx * mx + my * my) + inside - r;
}
static inline float covFromDist(float d) {
  float c = 0.5f - d;
  return c < 0 ? 0 : (c > 1 ? 1 : c);
}

void fillRoundRect(float x, float y, float w, float h, float r, uint16_t c, uint8_t corners) {
  if (w <= 0 || h <= 0) return;
  float rmax = (w < h ? w : h) * 0.5f;
  if (r > rmax) r = rmax;
  if (r < 0) r = 0;
  RRect q;
  q.cx = x + w * 0.5f;
  q.cy = y + h * 0.5f;
  q.hw = w * 0.5f;
  q.hh = h * 0.5f;
  q.rTL = (corners & CORNER_TL) ? r : 0;
  q.rTR = (corners & CORNER_TR) ? r : 0;
  q.rBR = (corners & CORNER_BR) ? r : 0;
  q.rBL = (corners & CORNER_BL) ? r : 0;
  int iy0 = (int)floorf(y), iy1 = (int)ceilf(y + h);
  for (int iy = iy0; iy < iy1; iy++) {
    if (iy < g_cy0 || iy >= g_cy1) continue;
    float yc = iy + 0.5f;
    bool top = yc < q.cy;
    float rl = top ? q.rTL : q.rBL, rr = top ? q.rTR : q.rBR;
    // left end: [pl, fl) pixel by pixel, from fl on full
    int pl, fl, fr, pr;
    float tl = top ? (y + rl) - yc : yc - (y + h - rl);
    if (rl > 0 && tl > 0) {
      float o = (rl + 0.5f) * (rl + 0.5f) - tl * tl;
      float in = (rl > 0.5f) ? (rl - 0.5f) * (rl - 0.5f) - tl * tl : -1;
      pl = (int)floorf(x + (o > 0 ? rl - sqrtf(o) : rl)) - 1;
      fl = (int)ceilf(x + (in > 0 ? rl - sqrtf(in) : rl)) + 1;
    } else {
      pl = (int)floorf(x) - 1;
      fl = (int)ceilf(x) + 1;
    }
    float tr = top ? (y + rr) - yc : yc - (y + h - rr);
    if (rr > 0 && tr > 0) {
      float o = (rr + 0.5f) * (rr + 0.5f) - tr * tr;
      float in = (rr > 0.5f) ? (rr - 0.5f) * (rr - 0.5f) - tr * tr : -1;
      pr = (int)ceilf(x + w - (o > 0 ? rr - sqrtf(o) : rr)) + 1;
      fr = (int)floorf(x + w - (in > 0 ? rr - sqrtf(in) : rr)) - 1;
    } else {
      pr = (int)ceilf(x + w) + 1;
      fr = (int)floorf(x + w) - 1;
    }
    if (fl > fr) fl = fr = (pl + pr) / 2;
    for (int ix = pl; ix < fl; ix++) plot(ix, iy, c, covFromDist(sdRRect(q, ix + 0.5f, yc)));
    float covMid = covFromDist(fabsf(yc - q.cy) - q.hh);  // top/bottom edge inside a pixel
    span(fl, fr, iy, c, (int)(covMid * 255.0f + 0.5f));
    for (int ix = fr; ix < pr; ix++) plot(ix, iy, c, covFromDist(sdRRect(q, ix + 0.5f, yc)));
  }
  addDirty((int)floorf(x) - 1, iy0, (int)ceilf(x + w) + 1, iy1);
}

void fillCircle(float cx, float cy, float r, uint16_t c) {
  fillRoundRect(cx - r, cy - r, 2 * r, 2 * r, r, c, CORNER_ALL);
}

void fillCircleOn(float cx, float cy, float r, uint16_t c, uint16_t base) {
  g_onBase = true;
  g_base = base;
  fillRoundRect(cx - r, cy - r, 2 * r, 2 * r, r, c, CORNER_ALL);
  g_onBase = false;
}

void frameRoundRect(float x, float y, float w, float h, float r, float border, uint16_t borderColor, uint16_t insideColor) {
  fillRoundRect(x, y, w, h, r, borderColor, CORNER_ALL);
  float ri = r - border;
  fillRoundRect(x + border, y + border, w - 2 * border, h - 2 * border, ri > 0 ? ri : 0, insideColor, CORNER_ALL);
}

// ------------------------------------------------------------ annular sectors
// Per pixel the signed distance to the shape's edge (inside negative): radially max(rIn - r, r - rOut);
// angularly the distance to the two end lines (sweep <= 180: inside both, > 180: inside either).
struct ArcBox {
  int x0, y0, x1, y1;
};
static ArcBox arcBox(float cx, float cy, float rIn, float rOut, float a0, float a1) {
  const float DEG = 3.14159265f / 180.0f;
  const float sweep = a1 - a0;
  float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
  auto take = [&](float x, float y) {
    if (x < bx0) bx0 = x;
    if (y < by0) by0 = y;
    if (x > bx1) bx1 = x;
    if (y > by1) by1 = y;
  };
  if (sweep >= 360.0f) {
    take(cx - rOut, cy - rOut);
    take(cx + rOut, cy + rOut);
  } else {
    int steps = (int)(sweep / 2.0f) + 2;
    for (int i = 0; i <= steps; i++) {
      float a = (a0 + sweep * (float)i / (float)steps) * DEG;
      take(cx + rOut * cosf(a), cy + rOut * sinf(a));
    }
    take(cx + rIn * cosf(a0 * DEG), cy + rIn * sinf(a0 * DEG));
    take(cx + rIn * cosf(a1 * DEG), cy + rIn * sinf(a1 * DEG));
  }
  ArcBox b = { (int)floorf(bx0) - 2, (int)floorf(by0) - 2, (int)ceilf(bx1) + 2, (int)ceilf(by1) + 2 };
  if (b.x0 < g_cx0) b.x0 = g_cx0;
  if (b.y0 < g_cy0) b.y0 = g_cy0;
  if (b.x1 > g_cx1) b.x1 = g_cx1;
  if (b.y1 > g_cy1) b.y1 = g_cy1;
  return b;
}

// mode 0 = smooth, 1 = hard, 2 = glow (alpha falls from rIn to rOut)
static void arcFill(float cx, float cy, float rIn, float rOut, float a0, float a1, uint16_t c, int mode, uint8_t glowAlpha) {
  float sweep = a1 - a0;
  if (sweep <= 0 || rOut <= 0 || rOut <= rIn) return;
  if (rIn < 0) rIn = 0;
  const float DEG = 3.14159265f / 180.0f;
  bool full = sweep >= 360.0f;
  float d0x = cosf(a0 * DEG), d0y = sinf(a0 * DEG), d1x = cosf(a1 * DEG), d1y = sinf(a1 * DEG);
  const ArcBox b = arcBox(cx, cy, rIn, rOut, a0, a1);
  const int ix0 = b.x0, iy0 = b.y0, ix1 = b.x1, iy1 = b.y1;
  const float ro = rOut + 1.0f, ri = rIn - 1.0f;
  const float inSq = (rIn + 1.0f) * (rIn + 1.0f), outSq = (rOut - 1.0f) * (rOut - 1.0f);  // 1 px away from the radial edges
  const float gw = rOut - rIn;
  for (int iy = iy0; iy < iy1; iy++) {
    float dy = iy + 0.5f - cy;
    float oo = ro * ro - dy * dy;
    if (oo <= 0) continue;
    float xo = sqrtf(oo);
    float ii = (ri > 0) ? ri * ri - dy * dy : -1;
    float xi = ii > 0 ? sqrtf(ii) : 0;
    // parts of the row on the ring: [cx - xo, cx - xi] and [cx + xi, cx + xo] (one part when xi = 0)
    for (int side = 0; side < (xi > 0 ? 2 : 1); side++) {
      float xa = (xi > 0) ? (side ? cx + xi : cx - xo) : cx - xo;
      float xb = (xi > 0) ? (side ? cx + xo : cx - xi) : cx + xo;
      int sx0 = (int)floorf(xa) - 1, sx1 = (int)ceilf(xb) + 1;
      if (sx0 < ix0) sx0 = ix0;
      if (sx1 > ix1) sx1 = ix1;
      for (int ix = sx0; ix < sx1; ix++) {
        float px = ix + 0.5f - cx, py = dy;
        float rr = px * px + py * py;
        float sd, rad = 0.0f;
        if (mode != 2 && rr > inSq && rr < outSq) {
          sd = -1.0f;  // middle of the ring: no square root
        } else {
          rad = sqrtf(rr);
          sd = rIn - rad;
          if (rad - rOut > sd) sd = rad - rOut;
        }
        if (!full) {
          float s0 = d0y * px - d0x * py;  // a0 line: inside negative
          float s1 = d1x * py - d1y * px;  // a1 line: inside negative
          float sa = (sweep <= 180.0f) ? (s0 > s1 ? s0 : s1) : (s0 < s1 ? s0 : s1);
          if (mode == 2) {  // glow: hard angular ends (neighbouring glows meet without a seam)
            if (sa >= 0) continue;
          } else if (sa > sd) {
            sd = sa;
          }
        }
        if (mode == 1) {
          if (sd < 0) plot(ix, iy, c, 1.0f);
        } else if (mode == 2) {
          float k = 1.0f - (rad - rIn) / gw;
          if (k <= 0 || rad < rIn) continue;
          if (k > 1) k = 1;
          plot(ix, iy, c, k * (float)glowAlpha / 255.0f);
        } else {
          plot(ix, iy, c, covFromDist(sd));
        }
      }
    }
  }
  addDirty(ix0, iy0, ix1, iy1);
}

void fillArc(float cx, float cy, float rIn, float rOut, float a0, float a1, uint16_t c) {
  arcFill(cx, cy, rIn, rOut, a0, a1, c, 0, 0);
}

void fillArcHard(float cx, float cy, float rIn, float rOut, float a0, float a1, uint16_t c) {
  arcFill(cx, cy, rIn, rOut, a0, a1, c, 1, 0);
}

void glowArc(float cx, float cy, float r0, float w, float a0, float a1, uint16_t c, uint8_t alpha) {
  arcFill(cx, cy, r0, r0 + w, a0, a1, c, 2, alpha);
}

// box and row spans as in arcFill; the colour of each pixel comes from the radial coverage and the coverage
// of the lit sector, written as is
void ringPaint(float cx, float cy, float rIn, float rOut, float a0, float a1, float litA0, float litA1, uint16_t lit, uint16_t track, uint16_t base) {
  float sweep = a1 - a0;
  if (sweep <= 0 || rOut <= 0 || rOut <= rIn) return;
  if (rIn < 0) rIn = 0;
  const float DEG = 3.14159265f / 180.0f;
  const ArcBox b = arcBox(cx, cy, rIn, rOut, a0, a1);
  const int ix0 = b.x0, iy0 = b.y0, ix1 = b.x1, iy1 = b.y1;
  const float ls = litA1 - litA0;
  const bool litAll = ls >= 360.0f, litNone = ls <= 0.0f;
  const float e0x = cosf(litA0 * DEG), e0y = sinf(litA0 * DEG), e1x = cosf(litA1 * DEG), e1y = sinf(litA1 * DEG);
  const float ro = rOut + 1.0f, ri = rIn - 1.0f;
  const float inSq = (rIn + 1.0f) * (rIn + 1.0f), outSq = (rOut - 1.0f) * (rOut - 1.0f);
  for (int iy = iy0; iy < iy1; iy++) {
    float dy = iy + 0.5f - cy;
    float oo = ro * ro - dy * dy;
    if (oo <= 0) continue;
    float xo = sqrtf(oo);
    float ii = (ri > 0) ? ri * ri - dy * dy : -1;
    float xi = ii > 0 ? sqrtf(ii) : 0;
    for (int side = 0; side < (xi > 0 ? 2 : 1); side++) {
      float xa = (xi > 0) ? (side ? cx + xi : cx - xo) : cx - xo;
      float xb = (xi > 0) ? (side ? cx + xo : cx - xi) : cx + xo;
      int sx0 = (int)floorf(xa) - 1, sx1 = (int)ceilf(xb) + 1;
      if (sx0 < ix0) sx0 = ix0;
      if (sx1 > ix1) sx1 = ix1;
      for (int ix = sx0; ix < sx1; ix++) {
        float px = ix + 0.5f - cx, py = dy;
        float rr = px * px + py * py;
        float sd;
        if (rr > inSq && rr < outSq) {
          sd = -1.0f;
        } else {
          float r = sqrtf(rr);
          sd = rIn - r;
          if (r - rOut > sd) sd = r - rOut;
        }
        const float cr = covFromDist(sd);
        if (cr <= 0) continue;  // outside the ring: untouched
        float cl = litAll ? 1.0f : 0.0f;
        if (!litAll && !litNone) {
          float s0 = e0y * px - e0x * py;
          float s1 = e1x * py - e1y * px;
          cl = covFromDist(ls <= 180.0f ? (s0 > s1 ? s0 : s1) : (s0 < s1 ? s0 : s1));
        }
        const int al = (int)(cl * 255.0f + 0.5f), ar = (int)(cr * 255.0f + 0.5f);
        const uint16_t ring = al >= 255 ? lit : (al <= 0 ? track : blend(track, lit, (uint32_t)al));
        g_fb[iy * W + ix] = ar >= 255 ? ring : blend(base, ring, (uint32_t)ar);
      }
    }
  }
  addDirty(ix0, iy0, ix1, iy1);
}

// ------------------------------------------------------------ polygons
// Every pixel accumulates the change of covered area where an edge passes; summing each row from the left
// gives the coverage. With a clip, only the rows inside the clip are kept (rows are independent). The
// accumulation still runs in the coordinates of the whole polygon and walks every row of an edge, so the
// kept rows get bit-identical values to an unclipped fill: a part redrawn inside a clip gives exactly the
// pixels of a full redraw. Columns are not cut; polyFill writes only the clip columns.
static Rect g_pb = { 0, 0, 0, 0 };  // bounds = origin of the accumulation
static int g_pw = 0, g_ph = 0;      // g_pw includes 2 spare columns on the right; g_ph = rows of the bounds
static int g_rowLo = 0, g_rowHi = 0;  // rows kept [g_rowLo, g_rowHi) (relative to g_pb.y0)

void polyBegin(Rect b) {
  if (b.x0 < 0) b.x0 = 0;
  if (b.y0 < 0) b.y0 = 0;
  if (b.x1 > W) b.x1 = (int16_t)W;
  if (b.y1 > H) b.y1 = (int16_t)H;
  g_pb = b;
  g_pw = (b.x1 - b.x0) + 2;
  g_ph = (b.y1 - b.y0);
  g_rowLo = (g_cy0 > b.y0 ? g_cy0 : b.y0) - b.y0;
  g_rowHi = (g_cy1 < b.y1 ? g_cy1 : b.y1) - b.y0;
  if (g_pw < 2 || g_ph <= 0 || g_rowHi <= g_rowLo || b.x1 <= g_cx0 || b.x0 >= g_cx1) {
    g_pw = 2;
    g_ph = 0;
    g_rowLo = g_rowHi = 0;
    return;
  }
  memset(g_acc, 0, sizeof(float) * (size_t)g_pw * (size_t)(g_rowHi - g_rowLo));
}

static void accLine(float ax, float ay, float bx, float by) {
  if (ay == by) return;
  float dir = 1.0f;
  if (ay > by) {
    dir = -1.0f;
    float t = ax;
    ax = bx;
    bx = t;
    t = ay;
    ay = by;
    by = t;
  }
  const float xmax = (float)(g_pw - 2);
  float dxdy = (bx - ax) / (by - ay);
  float x = ax;
  int ys = (int)floorf(ay);
  if (ay < 0) {
    x -= ay * dxdy;
    ys = 0;
  }
  int ye = (int)ceilf(by);
  if (ye > g_ph) ye = g_ph;
  if (ye > g_rowHi) ye = g_rowHi;  // rows below the clip are not needed
  for (int yy = ys; yy < ye; yy++) {
    float dy = fminf((float)(yy + 1), by) - fmaxf((float)yy, ay);
    float xnext = x + dxdy * dy;
    if (yy < g_rowLo) {  // above the clip: only follow the edge
      x = xnext;
      continue;
    }
    float *row = g_acc + (size_t)(yy - g_rowLo) * g_pw;
    float d = dy * dir;
    float x0 = x < xnext ? x : xnext, x1 = x < xnext ? xnext : x;
    if (x0 < 0) x0 = 0;
    if (x1 < 0) x1 = 0;
    if (x0 > xmax) x0 = xmax;
    if (x1 > xmax) x1 = xmax;
    float x0floor = floorf(x0);
    int x0i = (int)x0floor;
    float x1ceil = ceilf(x1);
    int x1i = (int)x1ceil;
    if (x1i <= x0i + 1) {
      float xmf = 0.5f * (x0 + x1) - x0floor;
      row[x0i] += d - d * xmf;
      row[x0i + 1] += d * xmf;
    } else {
      float s = 1.0f / (x1 - x0);
      float x0f = x0 - x0floor;
      float a0 = 0.5f * s * (1.0f - x0f) * (1.0f - x0f);
      float x1f = x1 - x1ceil + 1.0f;
      float am = 0.5f * s * x1f * x1f;
      row[x0i] += d * a0;
      if (x1i == x0i + 2) {
        row[x0i + 1] += d * (1.0f - a0 - am);
      } else {
        float a1 = s * (1.5f - x0f);
        row[x0i + 1] += d * (a1 - a0);
        for (int xi = x0i + 2; xi < x1i - 1; xi++) row[xi] += d * s;
        float a2 = a1 + (float)(x1i - x0i - 3) * s;
        row[x1i - 1] += d * (1.0f - a2 - am);
      }
      row[x1i] += d * am;
    }
    x = xnext;
  }
}

void polyAdd(const Pt *p, int n) {
  if (n < 3 || g_ph <= 0) return;
  float area2 = 0;
  for (int i = 0; i < n; i++) {
    const Pt &a = p[i], &b = p[(i + 1) % n];
    area2 += a.x * b.y - b.x * a.y;
  }
  float ox = (float)g_pb.x0, oy = (float)g_pb.y0;
  for (int i = 0; i < n; i++) {
    const Pt &a = p[i], &b = p[(i + 1) % n];
    if (area2 >= 0) accLine(a.x - ox, a.y - oy, b.x - ox, b.y - oy);  // same orientation: overlaps add up
    else accLine(b.x - ox, b.y - oy, a.x - ox, a.y - oy);
  }
}

void polyFill(uint16_t c) {
  if (g_ph <= 0) return;
  int bw = g_pw - 2;
  const int xEnd = g_cx1 - g_pb.x0 < bw ? g_cx1 - g_pb.x0 : bw;  // columns right of the clip are not needed
  for (int yy = g_rowLo; yy < g_rowHi; yy++) {
    int y = g_pb.y0 + yy;
    const float *row = g_acc + (size_t)(yy - g_rowLo) * g_pw;
    float acc = 0;
    uint16_t *line = g_fb + y * W;
    for (int xx = 0; xx < xEnd; xx++) {
      acc += row[xx];
      float v = fabsf(acc);
      if (v < 0.002f) continue;
      int x = g_pb.x0 + xx;
      if (x < g_cx0) continue;
      int a = v >= 0.998f ? 255 : (int)(v * 255.0f + 0.5f);
      a = withAlpha(a);
      if (a <= 0) continue;
      uint16_t *px = line + x;
      if (a >= 255) *px = c;
      else *px = blend(*px, c, (uint32_t)a);
    }
  }
  addDirty(g_pb.x0, g_pb.y0 + g_rowLo, g_pb.x1, g_pb.y0 + g_rowHi);  // cut to the clip in addDirty
}

void polyToMask(uint8_t *mask) {  // rows of the bounds that are inside the clip (all rows without a clip)
  int bw = g_pw - 2;
  for (int yy = g_rowLo; yy < g_rowHi; yy++) {
    const float *row = g_acc + (size_t)(yy - g_rowLo) * g_pw;
    float acc = 0;
    for (int xx = 0; xx < bw; xx++) {
      acc += row[xx];
      float v = fabsf(acc);
      if (v > 1.0f) v = 1.0f;
      mask[yy * bw + xx] = (uint8_t)(v * 255.0f + 0.5f);
    }
  }
}

void maskReplace(const uint8_t *mask, Rect r, bool mirrorX, uint16_t c, uint16_t base) {
  int bw = r.x1 - r.x0;
  for (int y = r.y0; y < r.y1; y++) {
    if (y < g_cy0 || y >= g_cy1) continue;
    const uint8_t *m = mask + (y - r.y0) * bw;
    for (int i = 0; i < bw; i++) {
      uint8_t a = m[i];
      if (!a) continue;
      int x = mirrorX ? (W - 1 - (r.x0 + i)) : (r.x0 + i);
      if (x < g_cx0 || x >= g_cx1) continue;
      g_fb[y * W + x] = (a == 255) ? c : blend(base, c, a);
    }
  }
  if (mirrorX) addDirty(W - r.x1, r.y0, W - r.x0, r.y1);
  else addDirty(r.x0, r.y0, r.x1, r.y1);
}

Rect ptsRect(const Pt *p, int n) {
  float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  for (int i = 0; i < n; i++) {
    if (p[i].x < x0) x0 = p[i].x;
    if (p[i].y < y0) y0 = p[i].y;
    if (p[i].x > x1) x1 = p[i].x;
    if (p[i].y > y1) y1 = p[i].y;
  }
  int ix0 = (int)floorf(x0) - 2, iy0 = (int)floorf(y0) - 2, ix1 = (int)ceilf(x1) + 2, iy1 = (int)ceilf(y1) + 2;
  if (ix0 < 0) ix0 = 0;
  if (iy0 < 0) iy0 = 0;
  if (ix1 > W) ix1 = W;
  if (iy1 > H) iy1 = H;
  if (ix1 < ix0) ix1 = ix0;
  if (iy1 < iy0) iy1 = iy0;
  return Rect{ (int16_t)ix0, (int16_t)iy0, (int16_t)ix1, (int16_t)iy1 };
}

void fillPoly(const Pt *p, int n, uint16_t c) {
  if (n < 3) return;
  polyBegin(ptsRect(p, n));
  polyAdd(p, n);
  polyFill(c);
}

void fillQuad(float x0, float y0, float x1, float y1, float w, uint16_t c) {
  float dx = x1 - x0, dy = y1 - y0;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 1e-3f) return;
  float nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
  Pt p[4] = { { x0 + nx, y0 + ny }, { x1 + nx, y1 + ny }, { x1 - nx, y1 - ny }, { x0 - nx, y0 - ny } };
  fillPoly(p, 4, c);
}

// ------------------------------------------------------------ text and icons
void setTabularDigits(bool tabular) {
  g_tabular = tabular;
}
void setTracking(float px) {
  g_track = px;
}
uint32_t missingGlyphs() {
  return g_missing;
}

uint32_t nextCode(const char *&s) {
  uint8_t c = (uint8_t)*s;
  if (c == 0) return 0;
  s++;
  if (c < 0x80) return c;
  int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
  uint32_t code = c & (0x3F >> extra);
  if (extra == 0) return 0xFFFD;  // stray continuation byte: drawn as a box
  while (extra-- > 0) {
    uint8_t d = (uint8_t)*s;
    if ((d & 0xC0) != 0x80) return 0xFFFD;  // cut-off character
    s++;
    code = (code << 6) | (d & 0x3F);
  }
  return code;
}

static const GlyphInfo *lookup(const FontData &f, uint32_t code) {
  int lo = 0, hi = (int)f.count - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    uint32_t c = f.glyphs[mid].code;
    if (c == code) return &f.glyphs[mid];
    if (c < code) lo = mid + 1;
    else hi = mid - 1;
  }
  return nullptr;
}
static const GlyphInfo *findGlyph(const FontData &f, uint32_t code) {
  if (!g_tabular && code >= '0' && code <= '9') {
    const GlyphInfo *g = lookup(f, FONT_PROP_DIGIT_BASE + (code - '0'));
    if (g) return g;
  }
  return lookup(f, code);
}

// draws a 4-bit coverage image folded with RLE in colour c at (x0, y0)
static void blitRle(const uint8_t *p, int w, int h, int x0, int y0, uint16_t c) {
  int total = w * h, i = 0, col = 0, row = 0;
  while (i < total) {
    uint8_t b = *p++;
    int v = b & 15, run = (b >> 4) + 1;
    if ((b >> 4) == 15) run = 16 + *p++;
    i += run;
    if (v == 0) {
      col += run;
      while (col >= w) {
        col -= w;
        row++;
      }
      continue;
    }
    const int a = withAlpha(v * 17);
    while (run-- > 0) {
      int x = x0 + col, y = y0 + row;
      if (x >= g_cx0 && x < g_cx1 && y >= g_cy0 && y < g_cy1 && a > 0) {
        uint16_t *px = g_fb + y * W + x;
        *px = (a >= 255) ? c : blend(*px, c, (uint32_t)a);
      }
      if (++col >= w) {
        col = 0;
        row++;
      }
    }
  }
}

static inline float missingAdvance(const FontData &f) {
  return (float)f.px * 0.6f;
}

bool hasGlyph(const FontData &f, uint32_t code) {
  return findGlyph(f, code) != nullptr || code == ' ';
}

bool hasGlyphs(const FontData &f, const char *utf8) {
  const char *s = utf8;
  for (uint32_t code; (code = nextCode(s)) != 0;)
    if (!findGlyph(f, code)) return false;
  return true;
}

float glyphAdvance(const FontData &f, uint32_t code) {
  const GlyphInfo *g = findGlyph(f, code);
  return (g ? (float)g->adv16 / 16.0f : missingAdvance(f)) + g_track;
}

float textWidth(const FontData &f, const char *utf8) {
  float wsum = 0;
  int n = 0;
  const char *s = utf8;
  for (uint32_t code; (code = nextCode(s)) != 0; n++) wsum += glyphAdvance(f, code);
  return n ? wsum - g_track : 0.0f;  // no spacing after the last glyph
}

float drawGlyph(const FontData &f, float x, float baseline, uint32_t code, uint16_t c) {
  const GlyphInfo *g = findGlyph(f, code);
  const int by = (int)lroundf(baseline);
  if (!g) {  // missing: box outline, so that it is seen
    g_missing++;
    const float bw = missingAdvance(f) - 2.0f, bh = (float)(f.capH > 4 ? f.capH : f.px * 2 / 3);
    const int x0 = (int)lroundf(x) + 1, y0 = by - (int)bh;
    fillRect(x0, y0, (int)bw, 1, c);
    fillRect(x0, by - 1, (int)bw, 1, c);
    fillRect(x0, y0, 1, (int)bh, c);
    fillRect(x0 + (int)bw - 1, y0, 1, (int)bh, c);
    return x + missingAdvance(f) + g_track;
  }
  if (g->w && g->h) {
    int gx = (int)lroundf(x) + g->xoff, gy = by - g->yoff;
    blitRle(f.rle + g->offset, g->w, g->h, gx, gy, c);
    addDirty(gx, gy, gx + g->w, gy + g->h);
  }
  return x + (float)g->adv16 / 16.0f + g_track;
}

float drawText(const FontData &f, float x, float baseline, const char *utf8, uint16_t c) {
  const char *s = utf8;
  for (uint32_t code; (code = nextCode(s)) != 0;) x = drawGlyph(f, x, baseline, code, c);
  return *utf8 ? x - g_track : x;
}

void drawIcon(const IconData &ic, int x, int y, uint16_t c) {
  blitRle(ic.rle, ic.w, ic.h, x, y, c);
  addDirty(x, y, x + ic.w, y + ic.h);
}

}  // namespace gfx
}  // namespace ui
