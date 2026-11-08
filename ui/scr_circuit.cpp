// ui/scr_circuit.cpp - circuit chronograph (UI設計 §4) with the pre-run checklist (§6) and the shift flash
//
// Drawing: the screen is a list of items (ring segments, triangles, dial, numbers, right column ...) drawn
// bottom-up. Each item has a box and a key (what it shows). When keys change, the boxes are redrawn bottom-up
// with the clip set to them, into the cached layer (everything except the hands); the box is then copied to the
// frame buffer and the hands that cross it are drawn on top. Hands that moved are erased by copying their old and
// new pieces back from the layer and drawing the hands in those pieces again. Any frame therefore has exactly the
// pixels of a full redraw (the host test compares the two).
// Without a layer (begin(..., nullptr)) the boxes are redrawn directly in the frame buffer (same pixels).
// Shift flash: the items are always drawn on black. In the red phase, the black pixels outside the dial face and
// the clock face are shown red while the layer is copied to the frame buffer (present()), so a phase change only
// copies the area outside the dial face again; nothing is redrawn. Dark panels that must stay dark in the red
// phase (lap time plate, triangle edges) use near-black colours.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_int.h"

namespace ui {
namespace in {

static const float PI_F = 3.14159265f;
static const float DEG = PI_F / 180.0f;

// ---- geometry (UI設計 §4.2: dial centre (216, 225), diameter 400; right column x 436..584)
static const float CX = 216.0f, CY = 225.0f;
static const float RING_IN = 166.0f, RING_OUT = 200.0f, GLOW_W = 8.0f;
static const float FACE_R = 166.0f;  // black dial face; stays black while the background flashes
static const int SEGS = 20;          // per side, 100 rpm = 7 deg each
static const float SEG_PITCH = 7.0f, SEG_GAP = 1.5f;
static const float TICK_OUT = 160.0f, TICK_IN = 148.0f, TICK5_IN = 142.0f;
static const float NUM_R = 120.0f;
static const float SUB_CX = 216.0f, SUB_CY = 295.0f, SUB_R = 32.0f;
static const float LAPT_CY = 406.0f;  // lap time under the ring (200, 381) in dial coordinates
static const float CLK_CX = 510.0f, CLK_CY = 86.0f, CLK_R = 70.0f;
static const float TRI_TIP_R = 176.0f, TRI_BASE_R = 198.0f, TRI_BASE_W = 24.0f;
static const float PANEL_X = 46, PANEL_Y = 252, PANEL_W = 340, PANEL_H = 140, PANEL_H_READY = 48;
// square inside the dial face: nothing in it depends on the background colour
static const int FACE_SQ = (int)(FACE_R * 0.7071f) - 2;
static const uint16_t C_NEAR_BLACK = rgb(0x0A0A0A);  // stays dark in the red flash phase

// clock angle (0 = 12 o'clock, clockwise) -> gfx angle (0 = 3 o'clock, clockwise)
static inline float gdeg(float clockDeg) {
  return clockDeg - 90.0f;
}
static inline gfx::Pt polar(float cx, float cy, float r, float clockDeg) {
  const float a = gdeg(clockDeg) * DEG;
  return gfx::Pt{ cx + r * cosf(a), cy + r * sinf(a) };
}
static gfx::Rect arcRect(float cx, float cy, float rIn, float rOut, float c0, float c1) {  // clock angles
  gfx::Pt p[48];
  int n = 0;
  int steps = (int)((c1 - c0) / 3.0f) + 2;
  if (steps > 40) steps = 40;
  for (int i = 0; i <= steps; i++) p[n++] = polar(cx, cy, rOut, c0 + (c1 - c0) * (float)i / (float)steps);
  p[n++] = polar(cx, cy, rIn, c0);
  p[n++] = polar(cx, cy, rIn, c1);
  return gfx::ptsRect(p, n);
}
static gfx::Rect discRect(float cx, float cy, float r) {
  return rectXY((int)floorf(cx - r) - 2, (int)floorf(cy - r) - 2, (int)ceilf(cx + r) + 2, (int)ceilf(cy + r) + 2);
}

// ---- items, bottom to top
enum {
  IT_BG = 0,
  IT_FACE,
  IT_SEG0,                    // 40 ring segments: IT_SEG0 + side * SEGS + k (side 0 = left)
  IT_TRI_L = IT_SEG0 + 2 * SEGS,
  IT_TRI_R,
  IT_TICKS,
  IT_NUMS,
  IT_SUB,
  IT_DIALLAP,
  IT_PEAKTAG,
  IT_PEAKNUM,
  IT_LAPTIME,
  IT_PANEL,
  IT_DIVIDER,
  IT_CLOCK,
  IT_CLOCKDIGI,
  IT_LAPNO,
  IT_LAST,
  IT_BEST,
  IT_DELTA,
  IT_DELTAV,
  IT_SPEED,
  IT_PILLS,
  IT_COUNT
};
// hands (frame buffer only), bottom to top
enum { H_SUBGHOST = 0, H_SUBMIN, H_GHOST, H_SEC, H_CLKH, H_CLKM, H_COUNT };

enum SegColour : uint8_t { SG_GREEN_DK, SG_YELLOW_DK, SG_RED_DK, SG_GREEN, SG_YELLOW, SG_RED, SG_WHITE, SG_WHITE_DIM };
static const uint16_t SEG_RGB[] = { C_RING_GREEN_DK, C_RING_YELLOW_DK, C_RING_RED_DK, C_GREEN, C_YELLOW, C_RED, C_WHITE, C_WHITE_DIM };

struct View {
  bool red;     // red flash phase
  uint16_t bg;  // red phase colour (used by present())
  uint8_t seg[SEGS];  // SegColour per step (both sides alike)
  int triQ;           // marker offset from the ring start [1/4 deg], -1 none
  uint32_t key[IT_COUNT];
  gfx::Rect box[IT_COUNT];
  int16_t hand[H_COUNT];  // angle [1/4 deg], -1 hidden
  // texts
  char peak[8];
  uint16_t peakCol;
  char lapTime[16];
  bool lapRun;
  char clockDigi[8];
  char lapNo[8];
  char sector[8];
  char last[16];
  char best[16];
  char delta[16];
  uint16_t deltaCol, deltaFill;
  char speed[64];
  bool panel, ready;
  uint8_t pending;
};
static View s_old;
static bool s_haveOld = false;
static const Model *s_m = nullptr;  // model of the frame being drawn

static uint8_t bandOf(const ShiftPoints &sp, int rpm) {  // 0 green, 1 yellow, 2 red
  return rpm < sp.yellow ? 0 : (rpm < sp.red ? 1 : 2);
}
static uint16_t bandColour(const ShiftPoints &sp, int rpm) {
  if (rpm < sp.green) return C_WHITE;
  const uint8_t b = bandOf(sp, rpm);
  return b == 0 ? C_GREEN : (b == 1 ? C_YELLOW : C_RED);
}

// clock angles of segment k: left side (side 0) from 7:20 (220 deg) clockwise, right side mirrored from 4:40
static void segAngles(int side, int k, bool glow, float *c0, float *c1) {
  const float gap = glow ? 0.0f : SEG_GAP * 0.5f;
  if (side == 0) {
    *c0 = 220.0f + SEG_PITCH * (float)k + gap;
    *c1 = 220.0f + SEG_PITCH * (float)(k + 1) - gap;
  } else {
    *c0 = 140.0f - SEG_PITCH * (float)(k + 1) + gap;
    *c1 = 140.0f - SEG_PITCH * (float)k - gap;
  }
}
static gfx::Rect segRect(int side, int k) {  // fixed: computed once
  static gfx::Rect box[2][SEGS];
  static bool ready = false;
  if (!ready) {
    for (int sd = 0; sd < 2; sd++)
      for (int j = 0; j < SEGS; j++) {
        float c0, c1;
        segAngles(sd, j, true, &c0, &c1);
        box[sd][j] = arcRect(CX, CY, RING_IN, RING_OUT + GLOW_W, c0, c1);
      }
    ready = true;
  }
  return box[side][k];
}
static void drawSeg(int side, int k, uint8_t col) {
  float c0, c1;
  const uint16_t c = SEG_RGB[col];
  if (col >= SG_GREEN && col < SG_WHITE) {  // lit: glow outside the ring first (a wider transparent arc, UI設計 §4.1)
    segAngles(side, k, true, &c0, &c1);
    gfx::glowArc(CX, CY, RING_OUT - 0.5f, GLOW_W, gdeg(c0), gdeg(c1), c, col == SG_WHITE_DIM ? 60 : 120);
  }
  segAngles(side, k, false, &c0, &c1);
  gfx::fillArc(CX, CY, RING_IN, RING_OUT, gdeg(c0), gdeg(c1), c);
}

// peak triangles: tip towards the centre at r 176, base 24 px at r 198, black edge
static void triGeom(int side, int q, gfx::Pt *tip, gfx::Pt *base) {
  const float off = (float)q * 0.25f;
  const float a = side == 0 ? 220.0f + off : 140.0f - off;
  *tip = polar(CX, CY, TRI_TIP_R, a);
  *base = polar(CX, CY, TRI_BASE_R, a);
}
static gfx::Rect triRect(int side, int q) {
  gfx::Pt t, b;
  triGeom(side, q, &t, &b);
  return triangleRect(t, b, TRI_BASE_W, 1.5f);
}

// ---- hands
struct HandSpec {
  float cx, cy, rTail, rTip, w, edge;  // edge = black outline width (0 = none)
  uint16_t colour;
  uint8_t alpha;
  float hubR, hubEdge;  // hub disc (0 = none)
  uint16_t hubColour;
};
static const HandSpec HANDS[H_COUNT] = {
  { SUB_CX, SUB_CY, -4.0f, 22.0f, 3.0f, 0.0f, C_BLUE, 140, 0, 0, 0 },                // sub-dial ghost minutes
  { SUB_CX, SUB_CY, -4.0f, 22.0f, 3.0f, 0.0f, C_WHITE, 255, 3.0f, 0.0f, C_WHITE },   // sub-dial minutes 0..5
  { CX, CY, -30.0f, 150.0f, 8.0f, 0.0f, C_BLUE, 102, 0, 0, 0 },                      // ghost seconds (40 %)
  { CX, CY, -30.0f, 150.0f, 6.0f, 1.0f, C_YELLOW, 255, 6.0f, 2.0f, C_YELLOW },       // lap seconds, hub 12 px
  { CLK_CX, CLK_CY, -8.0f, 36.0f, 7.0f, 2.0f, C_WHITE, 255, 0, 0, 0 },               // clock hours
  { CLK_CX, CLK_CY, -8.0f, 56.0f, 5.0f, 2.0f, C_YELLOW, 255, 5.0f, 2.0f, C_WHITE },  // clock minutes, hub 10 px
};
// rounded rectangle along the hand (angle a [1/4 deg] clock), radius r0..r1, width w
static int handPoly(const HandSpec &h, int q, float r0, float r1, float w, gfx::Pt *p) {
  const float a = gdeg((float)q * 0.25f) * DEG, ca = cosf(a), sa = sinf(a);
  const float hw = w * 0.5f;
  float rc = hw * 0.6f;
  auto put = [&](float u, float v, int k) { p[k] = gfx::Pt{ h.cx + u * ca - v * sa, h.cy + u * sa + v * ca }; };
  const float cu[4] = { r1 - rc, r1 - rc, r0 + rc, r0 + rc }, cv[4] = { -hw + rc, hw - rc, hw - rc, -hw + rc };
  const float a0[4] = { -90.0f, 0.0f, 90.0f, 180.0f };
  int n = 0;
  for (int c = 0; c < 4; c++)
    for (int j = 0; j <= 3; j++) {
      const float t = (a0[c] + 30.0f * (float)j) * DEG;
      put(cu[c] + rc * cosf(t), cv[c] + rc * sinf(t), n++);
    }
  return n;
}
static void drawHand(int i, int q) {
  const HandSpec &h = HANDS[i];
  gfx::Pt p[16];
  if (h.edge > 0) {
    int n = handPoly(h, q, h.rTail - h.edge, h.rTip + h.edge, h.w + 2 * h.edge, p);
    gfx::fillPoly(p, n, C_BG);
  }
  gfx::setAlpha(h.alpha);
  int n = handPoly(h, q, h.rTail, h.rTip, h.w, p);
  gfx::fillPoly(p, n, h.colour);
  gfx::setAlpha(255);
  if (h.hubR > 0) {
    if (h.hubEdge > 0) gfx::fillCircle(h.cx, h.cy, h.hubR + h.hubEdge, C_BG);
    gfx::fillCircle(h.cx, h.cy, h.hubR, h.hubColour);
  }
}
// pieces of a hand (rectangles that together cover every pixel it touches)
static int handPieces(int i, int q, gfx::Rect *out, int cap) {
  const HandSpec &h = HANDS[i];
  const float a = gdeg((float)q * 0.25f) * DEG, ca = cosf(a), sa = sinf(a);
  const float r0 = h.rTail - h.edge, r1 = h.rTip + h.edge, m = h.w * 0.5f + h.edge + 2.0f;
  const int nP = (int)ceilf((r1 - r0) / 28.0f);
  int n = 0;
  for (int k = 0; k < nP && n < cap; k++) {
    const float u0 = r0 + (r1 - r0) * (float)k / (float)nP, u1 = r0 + (r1 - r0) * (float)(k + 1) / (float)nP;
    const float x0 = h.cx + u0 * ca, y0 = h.cy + u0 * sa, x1 = h.cx + u1 * ca, y1 = h.cy + u1 * sa;
    out[n++] = rectXY((int)floorf(fminf(x0, x1) - m), (int)floorf(fminf(y0, y1) - m), (int)ceilf(fmaxf(x0, x1) + m), (int)ceilf(fmaxf(y0, y1) + m));
  }
  if (h.hubR > 0 && n < cap) out[n++] = discRect(h.cx, h.cy, h.hubR + h.hubEdge + 1.0f);
  return n;
}
static bool handCrosses(int i, int q, const gfx::Rect &r) {
  gfx::Rect pc[12];
  const int n = handPieces(i, q, pc, 12);
  for (int k = 0; k < n; k++)
    if (overlap(pc[k], r)) return true;
  return false;
}
static void drawHands(const View &v, const gfx::Rect &r) {
  for (int i = 0; i < H_COUNT; i++)
    if (v.hand[i] >= 0 && handCrosses(i, v.hand[i], r)) drawHand(i, v.hand[i]);
}

// ---- layout of the texts (fixed slots)
static const gfx::Rect R_LAPTIME = { 216 - 96, 380, 216 + 104, 432 };
static const gfx::Rect R_CLOCKDIGI = { 450, 158, 570, 178 };
static const gfx::Rect R_LAPNO = { 436, 180, 590, 216 };
static const gfx::Rect R_LAST = { 436, 216, 590, 262 };
static const gfx::Rect R_BEST = { 436, 264, 590, 308 };
static const gfx::Rect R_DELTA = { 432, 310, 592, 382 };
static const gfx::Rect R_DELTAV = { 442, 334, 576, 380 };
static const gfx::Rect R_SPEED = { 436, 383, 590, 401 };
static const gfx::Rect R_PILLS = { 436, 402, 590, 436 };

static float tagW(const char *lab, const FontData &num, const char *n) {
  return labelW(FONT_CP13, lab, 0.18f) + 6.0f + gfx::textWidth(num, n) + 20.0f;
}
// red slanted tag "LAP 7" with the left end at x (bottom edge)
static void lapTag(float x, float y, float h, const FontData &num, const char *n) {
  const float w = tagW("LAP", num, n);
  fillPara(x, y, w, h, h * 0.25f, C_RED);
  const float cy = y + h * 0.5f;
  float tx = label(FONT_CP13, x + 10.0f + h * 0.12f, cy + (float)FONT_CP13.capH * 0.5f, "LAP", C_WHITE, 0.18f);
  gfx::drawText(num, tx + 6.0f, cy + (float)num.capH * 0.5f, n, C_WHITE);
}
static float dialLapW(const char *n) {
  return tagW("LAP", FONT_NUM28, n);
}
static const float DIALLAP_Y = 128, DIALLAP_H = 26;
static const float PEAKTAG_Y = 164, PEAKTAG_H = 20;

static void drawPanel(const View &v) {
  const Model &m = *s_m;
  const float h = v.ready ? PANEL_H_READY : PANEL_H;
  gfx::frameRoundRect(PANEL_X, PANEL_Y, PANEL_W, h, 4, 2, C_YELLOW, C_PANEL);
  // header: PRE-RUN CHECK tag (yellow, black text) and the pending / READY badge
  const float ty = PANEL_Y + 12, th = 24;
  const float tw = labelW(FONT_CP13, "PRE-RUN CHECK", 0.2f) + 26;
  fillPara(PANEL_X + 14, ty, tw, th, th * 0.25f, C_YELLOW);
  label(FONT_CP13, PANEL_X + 27, ty + th * 0.5f + (float)FONT_CP13.capH * 0.5f, "PRE-RUN CHECK", C_BG, 0.2f);
  char b[24];
  if (v.ready) snprintf(b, sizeof b, "READY");
  else snprintf(b, sizeof b, "未完了 %u", (unsigned)v.pending);
  const float bw = mixWidth(T18, b) + 28;
  const float bx = PANEL_X + PANEL_W - 16 - bw;
  fillPara(bx, ty, bw, th, th * 0.25f, v.ready ? C_GREEN : C_RED);
  mixDrawC(T18, bx + bw * 0.5f + th * 0.12f, ty + th * 0.5f + (float)FONT_JP18.capH * 0.5f, b, v.ready ? C_BG : C_WHITE);
  if (v.ready) return;
  static const char *const NAMES[] = { "スポーツ ON", "ESC / ASR OFF", "油温 OK", "RaceChrono" };
  for (int i = 0; i < m.check.n && i < 4; i++) {
    const CheckItem &it = m.check.item[i];
    const float x = PANEL_X + 18 + (float)(i % 2) * 164, y = PANEL_Y + 52 + (float)(i / 2) * 32;
    fillPara(x, y, 18, 18, 4, it.ok ? C_GREEN : C_RED);
    if (it.ok) checkMark(x + 11, y + 9, 15, C_BG);
    else crossMark(x + 11, y + 9, 14, C_WHITE);
    const char *nm = it.id < 4 ? NAMES[it.id] : "?";
    mixDraw(T18, x + 28, y + 9 + (float)FONT_JP18.capH * 0.5f, nm, it.ok ? C_WHITE : C_RED_TEXT);
  }
  if (m.courseName[0]) {
    const TextSet sets[1] = { T18 };
    mixDrawFit(sets, 1, PANEL_X + 18, PANEL_Y + PANEL_H - 14, PANEL_W - 36, m.courseName, C_LABEL);
  }
}

static void drawClockFace() {
  gfx::fillCircle(CLK_CX, CLK_CY, CLK_R, C_WHITE);  // 3 px white frame
  gfx::fillCircle(CLK_CX, CLK_CY, CLK_R - 3.0f, C_BG);
  const gfx::Rect cl = gfx::clip();
  for (int i = 0; i < 60; i++) {
    const bool hr = i % 5 == 0;
    gfx::Pt a = polar(CLK_CX, CLK_CY, hr ? 55.0f : 61.0f, (float)i * 6.0f), b = polar(CLK_CX, CLK_CY, 66.0f, (float)i * 6.0f);
    gfx::Pt pp[2] = { a, b };
    if (!overlap(gfx::ptsRect(pp, 2), cl)) continue;
    gfx::fillQuad(a.x, a.y, b.x, b.y, hr ? 3.0f : 1.5f, hr ? C_WHITE : C_TICK);
  }
  static const char *const NUMS[4] = { "12", "3", "6", "9" };
  for (int i = 0; i < 4; i++) {
    gfx::Pt p = polar(CLK_CX, CLK_CY, 42.0f, (float)i * 90.0f);
    textC(FONT_NUM24, p.x, p.y + (float)FONT_NUM24.capH * 0.5f, NUMS[i], C_WHITE);
  }
}

static void drawItem(int id, const View &v) {
  const Model &m = *s_m;
  const gfx::Rect cl = gfx::clip();
  if (id >= IT_SEG0 && id < IT_SEG0 + 2 * SEGS) {
    const int side = (id - IT_SEG0) / SEGS, k = (id - IT_SEG0) % SEGS;
    drawSeg(side, k, v.seg[k]);
    return;
  }
  switch (id) {
    case IT_BG: gfx::fillAll(C_BG); break;
    case IT_FACE: gfx::fillCircle(CX, CY, FACE_R, C_BG); break;
    case IT_TRI_L:
    case IT_TRI_R: {
      gfx::Pt t, b;
      triGeom(id == IT_TRI_L ? 0 : 1, v.triQ, &t, &b);
      edgedTriangle(t, b, TRI_BASE_W, 1.5f, C_WHITE, C_NEAR_BLACK);
      break;
    }
    case IT_TICKS:
      for (int i = 0; i < 60; i++) {
        const bool five = i % 5 == 0;
        gfx::Pt a = polar(CX, CY, five ? TICK5_IN : TICK_IN, (float)i * 6.0f), b = polar(CX, CY, TICK_OUT, (float)i * 6.0f);
        gfx::Pt pp[2] = { a, b };
        if (!overlap(gfx::ptsRect(pp, 2), cl)) continue;
        gfx::fillQuad(a.x, a.y, b.x, b.y, five ? 4.5f : 2.0f, five ? C_WHITE : C_TICK);
      }
      break;
    case IT_NUMS: {
      static const char *const N[6] = { "60", "10", "20", "30", "40", "50" };
      for (int i = 0; i < 6; i++) {
        gfx::Pt p = polar(CX, CY, NUM_R, (float)i * 60.0f);
        textC(FONT_NUM28, p.x, p.y + (float)FONT_NUM28.capH * 0.5f, N[i], C_WHITE);
      }
      break;
    }
    case IT_SUB:
      gfx::fillCircle(SUB_CX, SUB_CY, SUB_R + 1.5f, C_OFF);
      gfx::fillCircle(SUB_CX, SUB_CY, SUB_R, C_PANEL);
      for (int i = 0; i < 5; i++) {
        gfx::Pt a = polar(SUB_CX, SUB_CY, 25.0f, (float)i * 72.0f), b = polar(SUB_CX, SUB_CY, 30.0f, (float)i * 72.0f);
        gfx::fillQuad(a.x, a.y, b.x, b.y, 2.5f, C_WHITE);
      }
      label(FONT_CP13, SUB_CX - labelW(FONT_CP13, "MIN", 0.2f) * 0.5f, SUB_CY + 19.0f, "MIN", C_LABEL, 0.2f);
      break;
    case IT_DIALLAP: {
      const float w = dialLapW(v.lapNo);
      lapTag(CX - w * 0.5f - DIALLAP_H * 0.125f, DIALLAP_Y, DIALLAP_H, FONT_NUM28, v.lapNo);
      break;
    }
    case IT_PEAKTAG: {
      const float w = labelW(FONT_CP13, "PEAK RPM", 0.2f) + 24.0f;
      fillPara(CX - w * 0.5f - PEAKTAG_H * 0.125f, PEAKTAG_Y, w, PEAKTAG_H, PEAKTAG_H * 0.25f, C_RED);
      label(FONT_CP13, CX - w * 0.5f + 12.0f, PEAKTAG_Y + PEAKTAG_H * 0.5f + (float)FONT_CP13.capH * 0.5f, "PEAK RPM", C_WHITE, 0.2f);
      break;
    }
    case IT_PEAKNUM: textC(FONT_NUM88, CX, CY + (float)FONT_NUM88.capH * 0.5f, v.peak, v.peakCol); break;
    case IT_LAPTIME: {
      const float h = 44, w = gfx::textWidth(FONT_NUM44, "0:00.00") + 34, s = h * 0.25f;
      const float x = CX - w * 0.5f - s * 0.5f, y = LAPT_CY - h * 0.5f;
      framePara(x, y, w, h, s, 1.0f, C_OFF, C_NEAR_BLACK);
      fillPara(x, y, 6, h, s, C_YELLOW);
      textC(FONT_NUM44, CX + 4.0f, LAPT_CY + (float)FONT_NUM44.capH * 0.5f, v.lapTime, v.lapRun ? C_WHITE : C_LABEL);
      break;
    }
    case IT_PANEL: drawPanel(v); break;
    case IT_DIVIDER: gfx::fillRect(427, 16, 1, 418, C_OFF); break;
    case IT_CLOCK: drawClockFace(); break;
    case IT_CLOCKDIGI: textC(FONT_NUM16, CLK_CX, 174, v.clockDigi, C_LABEL); break;
    case IT_LAPNO: {
      lapTag(436, 182, 32, FONT_NUM34, v.lapNo);
      if (v.sector[0]) mixDraw(T18, 436 + tagW("LAP", FONT_NUM34, v.lapNo) + 18, 198 + (float)FONT_CP18.capH * 0.5f, v.sector, C_LABEL);
      break;
    }
    case IT_LAST:
      label(FONT_CP13, 436, 229, "LAST", C_LABEL, 0.22f);
      gfx::drawText(FONT_NUM34, 436, 258, v.last, m.circuit.lastOk ? C_WHITE : C_LABEL);
      break;
    case IT_BEST:
      gfx::fillRect(436, 266, 4, 40, C_YELLOW);
      label(FONT_CP13, 448, 277, "BEST", C_YELLOW, 0.22f);
      gfx::drawText(FONT_NUM34, 448, 305, v.best, m.circuit.bestOk ? C_YELLOW : C_LABEL);
      break;
    case IT_DELTA: {  // frame and label
      const float x = 436, y = 312, w = 136, h = 68, s = 12;
      framePara(x, y, w, h, s, 2.0f, v.deltaCol, v.deltaFill);
      label(FONT_CP13, x + s + 12, y + 19, "VS BEST", C_SUB, 0.22f);
      break;
    }
    case IT_DELTAV: gfx::drawText(FONT_NUM48, 448, 371, v.delta, v.deltaCol); break;
    case IT_SPEED: mixDraw(T14, 436, 397, v.speed, C_LABEL); break;
    case IT_PILLS: {
      pill(436, 404, 46, 15, "LOG", statusColor(m.stLog), C_SUB);
      pill(487, 404, 46, 15, "CAN", statusColor(m.stCan), C_SUB);
      pill(538, 404, 46, 15, "BLE", statusColor(m.stBle), C_SUB);
      pill(436, 421, 52, 15, "WiFi", statusColor(m.stWifi), C_SUB);
      if (m.oil != OIL_UNKNOWN) pill(493, 421, 91, 15, m.oil == OIL_COLD ? "油温 COLD" : "油温 OK", m.oil == OIL_COLD ? C_BLUE : C_GREEN, C_SUB);
      break;
    }
    default: break;
  }
}

// ---- the view of a model
static void makeView(const Model &m, uint32_t now, View &v) {
  memset(&v, 0, sizeof v);
  const ShiftPoints &sp = m.shift;
  const bool red = flashPhase(m, now);
  g.flashRed = red;
  v.red = red;
  v.bg = flashBg(m);
  // ring
  for (int k = 0; k < SEGS; k++) {
    const int at = sp.green + 100 * k;
    const uint8_t b = bandOf(sp, at);
    if (m.flash) v.seg[k] = m.nightDim ? SG_WHITE_DIM : SG_WHITE;
    else if (m.rpmOk && m.rpm >= at) v.seg[k] = (uint8_t)(SG_GREEN + b);
    else v.seg[k] = (uint8_t)(SG_GREEN_DK + b);
  }
  v.triQ = -1;
  if (m.peakVisible && m.rpmOk) {
    float off = ((float)m.peakMarkerRpm - (float)sp.green) * SEG_PITCH / 100.0f;
    off = clampf(off, 0.0f, SEG_PITCH * SEGS);
    v.triQ = (int)lroundf(off * 4.0f);
  }
  // texts
  const Circuit &c = m.circuit;
  v.peakCol = C_WHITE;
  if (m.peakVisible && m.peakTopRpm > 0) {
    snprintf(v.peak, sizeof v.peak, "%u", (unsigned)(m.peakTopRpm > 9999 ? 9999 : m.peakTopRpm));
    v.peakCol = bandColour(sp, m.peakTopRpm);
  }
  v.lapRun = c.running;
  if (c.running) fmtLap(v.lapTime, sizeof v.lapTime, c.lapMs);
  else snprintf(v.lapTime, sizeof v.lapTime, "--:--.--");
  if (m.clock.ok) snprintf(v.clockDigi, sizeof v.clockDigi, "%02u:%02u", (unsigned)m.clock.hour, (unsigned)m.clock.minute);
  else snprintf(v.clockDigi, sizeof v.clockDigi, "--:--");
  if (c.lap > 0) snprintf(v.lapNo, sizeof v.lapNo, "%u", (unsigned)(c.lap > 999 ? 999 : c.lap));
  else snprintf(v.lapNo, sizeof v.lapNo, "--");
  if (c.lap > 0 && c.sector > 0) snprintf(v.sector, sizeof v.sector, "S%u", (unsigned)c.sector);
  if (c.lastOk) fmtLap(v.last, sizeof v.last, c.lastMs);
  else snprintf(v.last, sizeof v.last, "--:--.--");
  if (c.bestOk) fmtLap(v.best, sizeof v.best, c.bestMs);
  else snprintf(v.best, sizeof v.best, "--:--.--");
  if (c.deltaOk) {
    fmtDelta(v.delta, sizeof v.delta, c.deltaS, true);
    const bool fast = c.deltaS < 0;
    v.deltaCol = fast ? C_GREEN : C_RED;
    v.deltaFill = fast ? C_TINT_GREEN : C_TINT_RED2;
  } else {
    snprintf(v.delta, sizeof v.delta, "--");
    v.deltaCol = C_LABEL;
    v.deltaFill = C_PANEL;
  }
  char sp1[24], sp2[16];
  if (m.speedOk) snprintf(sp1, sizeof sp1, "%d km/h", (int)lroundf(clampf(m.speedKmh, 0.0f, 999.0f)));
  else snprintf(sp1, sizeof sp1, "-- km/h");
  if (m.gpsOk) {
    fmtTenths(sp2, sizeof sp2, m.hAccM);
    snprintf(v.speed, sizeof v.speed, "%s ・ GPS %s m", sp1, sp2);
  } else {
    snprintf(v.speed, sizeof v.speed, "%s ・ GPS --", sp1);
  }
  v.panel = m.check.show;
  v.pending = 0;
  for (int i = 0; i < m.check.n && i < 4; i++)
    if (!m.check.item[i].ok) v.pending++;
  v.ready = v.panel && v.pending == 0;
  // hands: lap seconds and minutes stop at 0 until S/F; the ghost only with a ghost time
  const uint32_t lapMs = c.running ? c.lapMs : 0;
  v.hand[H_SEC] = (int16_t)((lapMs % 60000u) * 1440u / 60000u);
  const uint32_t subQ = lapMs >= 300000u ? 0u : lapMs * 1440u / 300000u;  // 0..5 min, 72 deg per minute
  v.hand[H_SUBMIN] = (int16_t)subQ;
  if (c.running && c.ghostOk) {
    v.hand[H_GHOST] = (int16_t)((c.ghostMs % 60000u) * 1440u / 60000u);
    const uint32_t gq = c.ghostMs >= 300000u ? 0u : c.ghostMs * 1440u / 300000u;
    v.hand[H_SUBGHOST] = (int16_t)gq;
  } else {
    v.hand[H_GHOST] = -1;
    v.hand[H_SUBGHOST] = -1;
  }
  if (v.panel) {  // the checklist panel covers the minute sub-dial: its hands are under the panel (canvas order)
    v.hand[H_SUBMIN] = -1;
    v.hand[H_SUBGHOST] = -1;
  }
  if (m.clock.ok) {
    v.hand[H_CLKH] = (int16_t)(((m.clock.hour % 12u) * 120u + m.clock.minute * 2u) % 1440u);
    v.hand[H_CLKM] = (int16_t)((m.clock.minute * 24u) % 1440u);
  } else {
    v.hand[H_CLKH] = -1;
    v.hand[H_CLKM] = -1;
  }
  // keys and boxes
  const gfx::Rect none = { 0, 0, 0, 0 };
  for (int i = 0; i < IT_COUNT; i++) v.box[i] = none;
  v.box[IT_BG] = rectXY(0, 0, gfx::W, gfx::H);
  v.key[IT_BG] = 1;
  v.box[IT_FACE] = discRect(CX, CY, FACE_R);
  for (int side = 0; side < 2; side++)
    for (int k = 0; k < SEGS; k++) {
      v.box[IT_SEG0 + side * SEGS + k] = segRect(side, k);
      v.key[IT_SEG0 + side * SEGS + k] = v.seg[k];
    }
  if (v.triQ >= 0) {
    v.box[IT_TRI_L] = triRect(0, v.triQ);
    v.box[IT_TRI_R] = triRect(1, v.triQ);
  }
  v.key[IT_TRI_L] = v.key[IT_TRI_R] = (uint32_t)(v.triQ + 1);
  v.box[IT_TICKS] = discRect(CX, CY, TICK_OUT + 2);
  v.box[IT_NUMS] = discRect(CX, CY, NUM_R + 22);
  v.box[IT_SUB] = discRect(SUB_CX, SUB_CY, SUB_R + 2);
  {
    const float w = dialLapW(v.lapNo) + DIALLAP_H * 0.25f;
    v.box[IT_DIALLAP] = rectXY((int)(CX - w * 0.5f) - 3, (int)DIALLAP_Y - 2, (int)(CX + w * 0.5f) + 3, (int)(DIALLAP_Y + DIALLAP_H) + 2);
    v.key[IT_DIALLAP] = hashStr(kHash0, v.lapNo);
  }
  if (v.peak[0]) {  // the tag only with the number
    v.box[IT_PEAKTAG] = rectXY((int)CX - 60, (int)PEAKTAG_Y - 2, (int)CX + 60, (int)(PEAKTAG_Y + PEAKTAG_H) + 2);
    v.key[IT_PEAKTAG] = 1;
    const float w = gfx::textWidth(FONT_NUM88, v.peak);  // box of the text (italic: more room on the right)
    v.box[IT_PEAKNUM] = rectXY((int)(CX - w * 0.5f) - 10, (int)CY - FONT_NUM88.capH / 2 - 6, (int)(CX + w * 0.5f) + 14,
                               (int)CY + FONT_NUM88.capH / 2 + FONT_NUM88.descH + 4);
    v.key[IT_PEAKNUM] = hashU(hashStr(kHash0, v.peak), v.peakCol);
  }
  v.box[IT_LAPTIME] = R_LAPTIME;
  v.key[IT_LAPTIME] = hashU(hashStr(kHash0, v.lapTime), v.lapRun);
  if (v.panel) {
    v.box[IT_PANEL] = rectXY((int)PANEL_X - 1, (int)PANEL_Y - 1, (int)(PANEL_X + PANEL_W) + 1, (int)(PANEL_Y + (v.ready ? PANEL_H_READY : PANEL_H)) + 1);
    uint32_t h = hashU(kHash0, v.ready ? 2u : 1u);
    h = hashBytes(h, m.check.item, sizeof m.check.item[0] * (m.check.n > 4 ? 4 : m.check.n));
    h = hashStr(hashU(h, m.check.n), m.courseName);
    v.key[IT_PANEL] = h;
  }
  v.box[IT_DIVIDER] = rectXY(426, 15, 429, 435);
  v.box[IT_CLOCK] = discRect(CLK_CX, CLK_CY, CLK_R + 1);
  v.box[IT_CLOCKDIGI] = R_CLOCKDIGI;
  v.key[IT_CLOCKDIGI] = hashStr(kHash0, v.clockDigi);
  v.box[IT_LAPNO] = R_LAPNO;
  v.key[IT_LAPNO] = hashStr(hashStr(kHash0, v.lapNo), v.sector);
  v.box[IT_LAST] = R_LAST;
  v.key[IT_LAST] = hashStr(hashU(kHash0, c.lastOk), v.last);
  v.box[IT_BEST] = R_BEST;
  v.key[IT_BEST] = hashStr(hashU(kHash0, c.bestOk), v.best);
  v.box[IT_DELTA] = R_DELTA;
  v.key[IT_DELTA] = hashU(v.deltaCol, v.deltaFill);
  v.box[IT_DELTAV] = R_DELTAV;
  v.key[IT_DELTAV] = hashU(hashStr(kHash0, v.delta), v.deltaCol);
  v.box[IT_SPEED] = R_SPEED;
  v.key[IT_SPEED] = hashStr(kHash0, v.speed);
  v.box[IT_PILLS] = R_PILLS;
  v.key[IT_PILLS] = (uint32_t)m.stLog | ((uint32_t)m.stCan << 4) | ((uint32_t)m.stBle << 8) | ((uint32_t)m.stWifi << 12) | ((uint32_t)m.oil << 16);
}

// ---- redraw
// layer -> frame buffer for r (the clip is r). Red phase: black pixels outside the dial face and the clock face
// become the flash colour. Without a layer the items were drawn in the frame buffer: the same in place.
static void present(const View &v, const gfx::Rect &r) {
  uint16_t *fb = gfx::screen();
  const uint16_t *src = g.layer ? g.layer : fb;
  if (!v.red) {
    if (g.layer) gfx::copyRectFrom(g.layer, r.x0, r.y0, r.x1, r.y1);
    return;
  }
  const float fr2 = FACE_R * FACE_R, cr = CLK_R - 3.0f, cr2 = cr * cr;
  for (int y = r.y0; y < r.y1; y++) {
    // pixel-centre spans of the two discs on this row (kept as they are)
    int a0 = 1, a1 = 0, b0 = 1, b1 = 0;
    const float dy = (float)y + 0.5f - CY, q = fr2 - dy * dy;
    if (q > 0) {
      const float hw = sqrtf(q);
      a0 = (int)ceilf(CX - hw - 0.5f);
      a1 = (int)floorf(CX + hw - 0.5f);
    }
    const float dy2 = (float)y + 0.5f - CLK_CY, q2 = cr2 - dy2 * dy2;
    if (q2 > 0) {
      const float hw = sqrtf(q2);
      b0 = (int)ceilf(CLK_CX - hw - 0.5f);
      b1 = (int)floorf(CLK_CX + hw - 0.5f);
    }
    const uint16_t *s = src + y * gfx::W;
    uint16_t *d = fb + y * gfx::W;
    for (int x = r.x0; x < r.x1; x++) {
      const uint16_t c = s[x];
      d[x] = (c == 0 && !(x >= a0 && x <= a1) && !(x >= b0 && x <= b1)) ? v.bg : c;
    }
  }
  gfx::addDirty(r.x0, r.y0, r.x1, r.y1);
}
static void refreshScene(const View &v, const gfx::Rect &r) {
  gfx::setClip(r);
  if (g.layer) gfx::setTarget(g.layer);
  for (int i = 0; i < IT_COUNT; i++)
    if (overlap(v.box[i], r)) drawItem(i, v);
  gfx::setTarget(nullptr);
  present(v, r);
  drawHands(v, r);
  gfx::clearClip();
}
static void refreshHands(const View &v, const gfx::Rect &r) {
  if (!g.layer) {
    refreshScene(v, r);
    return;
  }
  gfx::setClip(r);
  present(v, r);
  drawHands(v, r);
  gfx::clearClip();
}

void circuitFrame(const Model &m, uint32_t now, bool full) {
  s_m = &m;
  static View v;
  makeView(m, now, v);
  if (full || !s_haveOld) {
    refreshScene(v, rectXY(0, 0, gfx::W, gfx::H));
    s_old = v;
    s_haveOld = true;
    return;
  }
  static RectList inv;
  inv.clear();
  static RectList hinv;
  hinv.clear();
  if (v.red != s_old.red || (v.red && v.bg != s_old.bg)) {  // flash phase: present everything outside the dial face again
    const int x0 = (int)CX - FACE_SQ, x1 = (int)CX + FACE_SQ, y0 = (int)CY - FACE_SQ, y1 = (int)CY + FACE_SQ;
    hinv.add(rectXY(0, 0, gfx::W, y0), 0);
    hinv.add(rectXY(0, y1, gfx::W, gfx::H), 0);
    hinv.add(rectXY(0, y0, x0, y1), 0);
    hinv.add(rectXY(x1, y0, gfx::W, y1), 0);
  }
  for (int i = IT_FACE; i < IT_COUNT; i++) {
    if (v.key[i] == s_old.key[i] && memcmp(&v.box[i], &s_old.box[i], sizeof(gfx::Rect)) == 0) continue;
    inv.add(s_old.box[i]);
    inv.add(v.box[i]);
  }
  for (int i = 0; i < inv.n; i++) refreshScene(v, inv.r[i]);
  // hands that moved: old and new pieces
  for (int i = 0; i < H_COUNT; i++) {
    if (v.hand[i] == s_old.hand[i]) continue;
    gfx::Rect pc[12];
    int n = s_old.hand[i] >= 0 ? handPieces(i, s_old.hand[i], pc, 12) : 0;
    for (int k = 0; k < n; k++) hinv.add(pc[k], 300);
    n = v.hand[i] >= 0 ? handPieces(i, v.hand[i], pc, 12) : 0;
    for (int k = 0; k < n; k++) hinv.add(pc[k], 300);
  }
  for (int i = 0; i < hinv.n; i++) refreshHands(v, hinv.r[i]);
  s_old = v;
}

}  // namespace in
}  // namespace ui
