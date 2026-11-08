// ui/scr_street.cpp - street screen: shift bar, water / fuel / speed tiles, maintenance, status row (UI設計 §5)
// Same redraw scheme as the circuit screen (items with boxes and keys, redrawn bottom-up inside the clip),
// directly in the frame buffer (nothing moves over the items, so no layer is needed).
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_int.h"

namespace ui {
namespace in {

// ---- geometry: bar y 0..44 (10 px top margin, 12 px left / right), tiles 44..236, maintenance 236..404, status 404..450
static const int NSEG = 20;
static const float BAR_X0 = 12, BAR_X1 = 588, BAR_Y0 = 16, BAR_Y1 = 42, BAR_SLANT = 9, BAR_SEGW = 24.5f;
static const float BAR_PITCH = (BAR_X1 - BAR_X0 - BAR_SLANT - BAR_SEGW) / (float)(NSEG - 1);
static const float TRI_Y0 = 10, TRI_Y1 = 28, TRI_W = 22;
static const float TILE_Y = 52, TILE_H = 176, TILE_W = 184;
static const float TILE_X[3] = { 16, 208, 400 };
static const float MROW_Y[2] = { 276, 342 }, MROW_H = 60;

enum {
  IT_BG = 0,
  IT_SEG0,
  IT_TRI = IT_SEG0 + NSEG,
  IT_TILE0,  // 3 tile frames with their labels
  IT_WVAL = IT_TILE0 + 3,
  IT_WBAR,
  IT_FVAL,
  IT_FRANGE,
  IT_FBAR,
  IT_SVAL,
  IT_RVAL,
  IT_MHEAD,
  IT_MROW0,
  IT_STATUS = IT_MROW0 + 2,
  IT_COUNT
};

struct View {
  uint16_t bg;
  uint8_t seg[NSEG];  // 0..2 dark green/yellow/red, 3..5 lit, 6 white, 7 dim white
  int tri;            // segment index of the peak marker, -1 none
  uint32_t key[IT_COUNT];
  gfx::Rect box[IT_COUNT];
};
static View s_old;
static bool s_haveOld = false;
static const Model *s_m = nullptr;
static const uint16_t SEG_RGB[] = { C_RING_GREEN_DK, C_RING_YELLOW_DK, C_RING_RED_DK, C_GREEN, C_YELLOW, C_RED, C_WHITE, C_WHITE_DIM };

static float segX(int i) {
  return BAR_X0 + BAR_PITCH * (float)i;
}
static gfx::Rect segRect(int i) {
  return rectXY((int)floorf(segX(i)) - 4, (int)BAR_Y0 - 4, (int)ceilf(segX(i) + BAR_SEGW + BAR_SLANT) + 4, (int)BAR_Y1 + 4);
}
static void drawSeg(int i, uint8_t col) {
  const float x = segX(i), h = BAR_Y1 - BAR_Y0;
  const uint16_t c = SEG_RGB[col];
  if (col >= 3) {  // lit: soft glow around
    gfx::setAlpha(col == 7 ? 40 : 70);
    fillPara(x - 3, BAR_Y0 - 3, BAR_SEGW + 6, h + 6, BAR_SLANT * (h + 6) / h, c);
    gfx::setAlpha(255);
  }
  fillPara(x, BAR_Y0, BAR_SEGW, h, BAR_SLANT, c);
}
// centre of segment i at height y (the segments lean right)
static float segCentreAt(int i, float y) {
  return segX(i) + BAR_SEGW * 0.5f + BAR_SLANT * (BAR_Y1 - y) / (BAR_Y1 - BAR_Y0);
}
static gfx::Rect triRect(int i) {
  const float cx = segCentreAt(i, (TRI_Y0 + TRI_Y1) * 0.5f);
  return rectXY((int)(cx - TRI_W * 0.5f) - 4, (int)TRI_Y0 - 4, (int)(cx + TRI_W * 0.5f) + 4, (int)TRI_Y1 + 4);
}
static void drawTri(int i) {
  const float cx = segCentreAt(i, (TRI_Y0 + TRI_Y1) * 0.5f);
  edgedTriangle(gfx::Pt{ cx, TRI_Y1 }, gfx::Pt{ cx, TRI_Y0 }, TRI_W, 1.5f, C_WHITE, C_BG);
}

static gfx::Rect tileRect(int t) {
  return rectXY((int)TILE_X[t] - 1, (int)TILE_Y - 1, (int)(TILE_X[t] + TILE_W) + 1, (int)(TILE_Y + TILE_H) + 1);
}
static void drawTile(int t) {
  static const char *const NAMES[3] = { "水温", "燃料", "速度" };
  static const uint16_t ACCENT[3] = { C_BLUE, C_YELLOW, C_RED };
  const float x = TILE_X[t];
  gfx::fillRoundRect(x, TILE_Y, TILE_W, TILE_H, 6, C_OFF);
  gfx::fillRoundRect(x + 1, TILE_Y + 1, TILE_W - 2, TILE_H - 2, 5, C_PANEL);
  gfx::fillRoundRect(x, TILE_Y, TILE_W, 4, 2, ACCENT[t], gfx::CORNER_TL | gfx::CORNER_TR);
  mixDraw(T18, x + 14, TILE_Y + 30, NAMES[t], C_LABEL);
}
static void bar(float x, float y, float w, float f, uint16_t c) {
  gfx::fillRoundRect(x, y, w, 10, 5, C_OFF);
  f = clampf(f, 0, 1);
  if (f > 0) gfx::fillRoundRect(x, y, w * f < 10 ? 10 : w * f, 10, 5, c);
}
// big number + unit at the left of a tile
static void valueUnit(float x, float base, const char *val, const char *unit, uint16_t c) {
  float e = gfx::drawText(FONT_NUM64, x, base, val, c);
  if (unit) mixDraw(T22, e + 6, base, unit, C_LABEL);
}
static uint16_t maintColour(uint8_t st) {
  return st == MS_OVERDUE ? C_RED : (st == MS_SOON ? C_YELLOW : C_GREEN);
}

static void drawMaintHead() {
  const Model &m = *s_m;
  const float y = 244, h = 24;
  const float tw = mixWidth(T18, "整備") + 26;
  fillPara(16, y, tw, h, h * 0.25f, C_ORANGE);
  mixDraw(T18, 29, y + h * 0.5f + (float)FONT_JP18.capH * 0.5f, "整備", C_BG);
  const float base = y + h * 0.5f + (float)FONT_JP18.capH * 0.5f;
  mixDraw(T18, 16 + tw + 14, base, "期限が近い順", C_LABEL);
  char o[24], t[16], s[96];
  if (m.maint.odoOk) fmtGrouped(o, sizeof o, m.maint.odoKm);
  else snprintf(o, sizeof o, "--");
  fmtTenths(t, sizeof t, m.maint.trackH);
  snprintf(s, sizeof s, "ODO %s km ・ サーキット %s h", o, t);
  mixDrawR(T18, 584, base, s, C_LABEL);
}
static void drawMaintRow(int r) {
  const Model &m = *s_m;
  const float y = MROW_Y[r];
  if (r >= m.maint.n) {
    if (r == 0) mixDraw(T18, 30, y + 36, "整備データなし", C_LABEL);
    return;
  }
  const MaintItem &it = m.maint.item[r];
  const uint16_t col = maintColour(it.state);
  gfx::fillRect(16, (int)y, 568, (int)MROW_H, it.state == MS_OVERDUE ? C_TINT_RED : C_PANEL);
  gfx::fillRect(16, (int)y, 4, (int)MROW_H, col);
  const float base = y + MROW_H * 0.5f + (float)FONT_JP28.capH * 0.5f;
  const float barX = 474, barW = 96;
  const float rw = mixWidth(T22, it.remain);
  const float rx = barX - 16 - rw;
  mixDraw(T22, rx, y + MROW_H * 0.5f + (float)FONT_JP22.capH * 0.5f, it.remain, col);
  const TextSet sets[2] = { T28, T22 };
  mixDrawFit(sets, 2, 32, base, rx - 16 - 32, it.name, C_WHITE);
  bar(barX, y + MROW_H * 0.5f - 5, barW, it.state == MS_OVERDUE ? 1.0f : it.fraction, col);
}

static void statusText(char *clock, size_t n, const Model &m) {
  if (m.clock.ok) snprintf(clock, n, "%02u:%02u", (unsigned)m.clock.hour, (unsigned)m.clock.minute);
  else snprintf(clock, n, "--:--");
}
static void drawStatus() {
  const Model &m = *s_m;
  const float base = 427 + (float)FONT_CP18.capH * 0.5f;
  char clock[8], gps[24], t[16];
  statusText(clock, sizeof clock, m);
  if (m.gpsOk) {
    fmtTenths(t, sizeof t, m.hAccM);
    snprintf(gps, sizeof gps, "GPS %s m", t);
  } else {
    snprintf(gps, sizeof gps, "GPS --");
  }
  const char *oil = m.oil == OIL_OK ? "油温 OK" : (m.oil == OIL_COLD ? "COLD" : nullptr);
  // widths: clock, 一般道, oil (dot + text), GPS, CAN, WiFi, BLE (dot + text)
  struct Part {
    const char *s;
    bool dot;
    uint16_t dotC, fg;
    bool num;
  } parts[7];
  int n = 0;
  parts[n++] = { clock, false, 0, C_WHITE, true };
  parts[n++] = { "一般道", false, 0, C_LABEL, false };
  if (oil) parts[n++] = { oil, true, m.oil == OIL_COLD ? C_BLUE : C_GREEN, m.oil == OIL_COLD ? C_BLUE : C_LABEL, false };
  parts[n++] = { gps, false, 0, C_LABEL, false };
  parts[n++] = { "CAN", true, statusColor(m.stCan), C_LABEL, false };
  parts[n++] = { "WiFi", true, statusColor(m.stWifi), C_LABEL, false };
  parts[n++] = { "BLE", true, statusColor(m.stBle), C_LABEL, false };
  float w[7], sum = 0;
  for (int i = 0; i < n; i++) {
    w[i] = (parts[i].num ? gfx::textWidth(FONT_NUM20, parts[i].s) : mixWidth(T18, parts[i].s)) + (parts[i].dot ? 16.0f : 0.0f);
    sum += w[i];
  }
  const float gap = (568.0f - sum) / (float)(n - 1);
  float x = 16;
  for (int i = 0; i < n; i++) {
    const Part &p = parts[i];
    float tx = x;
    if (p.dot) {
      gfx::fillCircle(x + 5, 427, 5, p.dotC);
      tx += 16;
    }
    if (p.num) gfx::drawText(FONT_NUM20, tx, 427 + (float)FONT_NUM20.capH * 0.5f, p.s, p.fg);
    else mixDraw(T18, tx, base, p.s, p.fg);
    x += w[i] + gap;
  }
}

static void drawItem(int id, const View &v) {
  const Model &m = *s_m;
  if (id >= IT_SEG0 && id < IT_SEG0 + NSEG) {
    drawSeg(id - IT_SEG0, v.seg[id - IT_SEG0]);
    return;
  }
  if (id >= IT_TILE0 && id < IT_TILE0 + 3) {
    drawTile(id - IT_TILE0);
    return;
  }
  if (id >= IT_MROW0 && id < IT_MROW0 + 2) {
    drawMaintRow(id - IT_MROW0);
    return;
  }
  char b[24];
  const float x0 = TILE_X[0] + 14, x1 = TILE_X[1] + 14, x2 = TILE_X[2] + 14;
  const float vbase = TILE_Y + 112;
  switch (id) {
    case IT_BG: gfx::fillAll(v.bg); break;
    case IT_TRI: drawTri(v.tri); break;
    case IT_WVAL:
      if (m.street.waterOk) snprintf(b, sizeof b, "%d", (int)lroundf(m.street.waterC));
      else snprintf(b, sizeof b, "--");
      valueUnit(x0, vbase, b, "°C", m.street.waterOk ? C_WHITE : C_LABEL);
      break;
    case IT_WBAR:
      if (m.street.waterOk) {
        const float c = m.street.waterC;
        bar(x0, TILE_Y + 150, TILE_W - 28, c / 130.0f, c >= 110.0f ? C_RED : (c < 60.0f ? C_BLUE : C_GREEN));
      } else {
        bar(x0, TILE_Y + 150, TILE_W - 28, 0, C_OFF);
      }
      break;
    case IT_FVAL:
      if (m.street.fuelOk) snprintf(b, sizeof b, "%d", (int)lroundf(m.street.fuelPct));
      else snprintf(b, sizeof b, "--");
      valueUnit(x1, vbase, b, "%", m.street.fuelOk ? C_WHITE : C_LABEL);
      break;
    case IT_FRANGE: {
      char s[32];
      if (m.street.rangeOk) snprintf(s, sizeof s, "約 %u km", (unsigned)m.street.rangeKm);
      else snprintf(s, sizeof s, "約 -- km");
      mixDraw(T18, x1, TILE_Y + 138, s, C_LABEL);
      break;
    }
    case IT_FBAR:
      bar(x1, TILE_Y + 150, TILE_W - 28, m.street.fuelOk ? m.street.fuelPct / 100.0f : 0, m.street.fuelPct <= 20.0f ? C_YELLOW : C_GREEN);
      break;
    case IT_SVAL:
      if (m.speedOk) snprintf(b, sizeof b, "%d", (int)lroundf(m.speedKmh < 0 ? 0 : m.speedKmh));
      else snprintf(b, sizeof b, "--");
      valueUnit(x2, vbase, b, "km/h", m.speedOk ? C_WHITE : C_LABEL);
      break;
    case IT_RVAL: {
      if (m.rpmOk) snprintf(b, sizeof b, "%u", (unsigned)m.rpm);
      else snprintf(b, sizeof b, "--");
      uint16_t c = C_LABEL;
      if (m.rpmOk) c = m.rpm >= m.shift.red ? C_RED : (m.rpm >= m.shift.yellow ? C_YELLOW : C_WHITE);
      float e = gfx::drawText(FONT_NUM34, x2, TILE_Y + 158, b, c);
      mixDraw(T18, e + 8, TILE_Y + 158, "rpm", C_LABEL);
      break;
    }
    case IT_MHEAD: drawMaintHead(); break;
    case IT_STATUS: drawStatus(); break;
    default: break;
  }
}

static void makeView(const Model &m, uint32_t now, View &v) {
  memset(&v, 0, sizeof v);
  const ShiftPoints &sp = m.shift;
  const bool red = flashPhase(m, now);
  g.flashRed = red;
  v.bg = red ? flashBg(m) : C_BG;
  for (int i = 0; i < NSEG; i++) {
    const int at = sp.green + 100 * i;
    const uint8_t b = at < sp.yellow ? 0 : (at < sp.red ? 1 : 2);
    if (m.flash) v.seg[i] = m.nightDim ? 7 : 6;
    else if (m.rpmOk && m.rpm >= at) v.seg[i] = (uint8_t)(3 + b);
    else v.seg[i] = b;
  }
  v.tri = -1;
  if (m.peakVisible && m.rpmOk) {
    int idx = ((int)m.peakMarkerRpm - (int)sp.green) / 100;
    v.tri = idx < 0 ? 0 : (idx >= NSEG ? NSEG - 1 : idx);
  }
  const gfx::Rect none = { 0, 0, 0, 0 };
  for (int i = 0; i < IT_COUNT; i++) v.box[i] = none;
  v.box[IT_BG] = rectXY(0, 0, gfx::W, gfx::H);
  v.key[IT_BG] = v.bg;
  for (int i = 0; i < NSEG; i++) {
    v.box[IT_SEG0 + i] = segRect(i);
    v.key[IT_SEG0 + i] = v.seg[i];
  }
  if (v.tri >= 0) v.box[IT_TRI] = triRect(v.tri);
  v.key[IT_TRI] = (uint32_t)(v.tri + 1);
  for (int t = 0; t < 3; t++) v.box[IT_TILE0 + t] = tileRect(t);
  const float x0 = TILE_X[0], x1 = TILE_X[1], x2 = TILE_X[2];
  v.box[IT_WVAL] = rectXY((int)x0 + 4, (int)TILE_Y + 40, (int)(x0 + TILE_W) - 2, (int)TILE_Y + 128);
  v.key[IT_WVAL] = hashU(m.street.waterOk, (uint32_t)lroundf(m.street.waterC));
  v.box[IT_WBAR] = rectXY((int)x0 + 10, (int)TILE_Y + 146, (int)(x0 + TILE_W) - 10, (int)TILE_Y + 164);
  v.key[IT_WBAR] = hashU(m.street.waterOk, (uint32_t)lroundf(m.street.waterC * 4));
  v.box[IT_FVAL] = rectXY((int)x1 + 4, (int)TILE_Y + 40, (int)(x1 + TILE_W) - 2, (int)TILE_Y + 128);
  v.key[IT_FVAL] = hashU(m.street.fuelOk, (uint32_t)lroundf(m.street.fuelPct));
  v.box[IT_FRANGE] = rectXY((int)x1 + 4, (int)TILE_Y + 120, (int)(x1 + TILE_W) - 2, (int)TILE_Y + 145);
  v.key[IT_FRANGE] = hashU(m.street.rangeOk, m.street.rangeKm);
  v.box[IT_FBAR] = rectXY((int)x1 + 10, (int)TILE_Y + 146, (int)(x1 + TILE_W) - 10, (int)TILE_Y + 164);
  v.key[IT_FBAR] = hashU(m.street.fuelOk, (uint32_t)lroundf(m.street.fuelPct * 4));
  v.box[IT_SVAL] = rectXY((int)x2 + 4, (int)TILE_Y + 40, (int)(x2 + TILE_W) - 2, (int)TILE_Y + 128);
  v.key[IT_SVAL] = hashU(m.speedOk, (uint32_t)lroundf(m.speedKmh));
  v.box[IT_RVAL] = rectXY((int)x2 + 4, (int)TILE_Y + 128, (int)(x2 + TILE_W) - 2, (int)TILE_Y + 166);
  v.key[IT_RVAL] = hashU(hashU(m.rpmOk, m.rpm), (uint32_t)m.shift.yellow << 16 | m.shift.red);
  v.box[IT_MHEAD] = rectXY(14, 240, 586, 272);
  v.key[IT_MHEAD] = hashU(hashU(hashU(kHash0, m.maint.odoOk), m.maint.odoKm), (uint32_t)lroundf(m.maint.trackH * 10));
  for (int r = 0; r < 2; r++) {
    v.box[IT_MROW0 + r] = rectXY(14, (int)MROW_Y[r] - 1, 586, (int)(MROW_Y[r] + MROW_H) + 1);
    uint32_t h = hashU(kHash0, (uint32_t)(r < m.maint.n) | ((uint32_t)m.maint.n << 1));
    if (r < m.maint.n) {
      const MaintItem &it = m.maint.item[r];
      h = hashStr(hashStr(h, it.name), it.remain);
      h = hashU(hashU(h, it.state), (uint32_t)lroundf(it.fraction * 1000));
    }
    v.key[IT_MROW0 + r] = h;
  }
  v.box[IT_STATUS] = rectXY(0, 406, gfx::W, gfx::H);
  char clock[8];
  statusText(clock, sizeof clock, m);
  uint32_t h = hashStr(kHash0, clock);
  h = hashU(h, (uint32_t)m.oil | ((uint32_t)m.stCan << 4) | ((uint32_t)m.stWifi << 8) | ((uint32_t)m.stBle << 12) | ((uint32_t)m.gpsOk << 16));
  v.key[IT_STATUS] = hashU(h, (uint32_t)lroundf(m.hAccM * 10));
}

static void refresh(const View &v, const gfx::Rect &r) {
  gfx::setClip(r);
  for (int i = 0; i < IT_COUNT; i++)
    if (overlap(v.box[i], r)) drawItem(i, v);
  gfx::clearClip();
}

void streetFrame(const Model &m, uint32_t now, bool full) {
  s_m = &m;
  static View v;
  makeView(m, now, v);
  if (full || !s_haveOld || v.bg != s_old.bg) {
    refresh(v, rectXY(0, 0, gfx::W, gfx::H));
    s_old = v;
    s_haveOld = true;
    return;
  }
  static RectList inv;
  inv.clear();
  for (int i = 1; i < IT_COUNT; i++) {
    if (v.key[i] == s_old.key[i] && memcmp(&v.box[i], &s_old.box[i], sizeof(gfx::Rect)) == 0) continue;
    inv.add(s_old.box[i]);
    inv.add(v.box[i]);
  }
  for (int i = 0; i < inv.n; i++) refresh(v, inv.r[i]);
  s_old = v;
}

}  // namespace in
}  // namespace ui
