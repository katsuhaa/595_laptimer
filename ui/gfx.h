// ui/gfx.h - drawing into the 600x450 RGB565 frame buffer.
//
// Port of the round gauge's renderer (abarth595-round-gauge meter_display/gfx.h, 466x466) to the square
// 600x450 screen. Same API and behaviour: anti-aliased fills, arcs, polygons, round rects, RLE 4-bit fonts,
// dirty-rect tracking and setTarget() for cached layers. Added for the square gadget:
//   - a clip rectangle (setClip): every function writes only inside it, so any part of a screen can be
//     redrawn from the bottom up and gives exactly the pixels of a full redraw;
//   - a global opacity (setAlpha) for the semi-transparent ghost hands and glows;
//   - fillPoly / fillQuad helpers, text tracking (letter spacing), a visible box for missing glyphs.
// No ESP32 dependency and no heap: the caller passes the frame buffer and the polygon scratch buffer.
#pragma once
#include <stdint.h>

#include "fonts_data.h"

namespace ui {
namespace gfx {

const int W = 600;
const int H = 450;

struct Rect {
  int16_t x0, y0, x1, y1;  // x1, y1 exclusive
};
struct Pt {
  float x, y;
};

// framebuffer: W*H uint16_t. scratch: W*(H+2) floats (polygon coverage accumulation)
void begin(uint16_t *framebuffer, float *scratch);
uint16_t *frame();   // the current target
uint16_t *screen();  // the frame buffer given to begin()
// Draw into another W x H buffer (a cached layer); nullptr = back to the frame buffer. While the target is
// not the frame buffer, nothing is added to the dirty list.
void setTarget(uint16_t *fb);

// Clip rectangle for every write (default: the whole screen). Kept until changed.
void setClip(int x0, int y0, int x1, int y1);
void setClip(const Rect &r);
void clearClip();
Rect clip();

// Opacity multiplier (0..255) for the following fills, polygons, arcs and text (default 255).
// Not applied by copyRectFrom/copyDiscFrom, ringPaint and maskReplace (they write absolute colours).
void setAlpha(uint8_t a);

// 0xRRGGBB -> RGB565 (rounded)
static constexpr uint16_t rgb(uint32_t c) {
  return (uint16_t)((((((c >> 16) & 0xFF) * 31 + 127) / 255) << 11) | (((((c >> 8) & 0xFF) * 63 + 127) / 255) << 5) | (((c & 0xFF) * 31 + 127) / 255));
}
// mix bg and fg by a (0..255)
static inline uint16_t blend(uint16_t bg, uint16_t fg, uint32_t a) {
  uint32_t k = (a + 2) >> 2;  // 0..64
  uint32_t ik = 64 - k;
  uint32_t rb = ((((uint32_t)bg & 0xF81F) * ik + ((uint32_t)fg & 0xF81F) * k) >> 6) & 0xF81F;
  uint32_t g = ((((uint32_t)bg & 0x07E0) * ik + ((uint32_t)fg & 0x07E0) * k) >> 6) & 0x07E0;
  return (uint16_t)(rb | g);
}

// ---- fills ----
void fillAll(uint16_t c);  // the whole clip rectangle
// copy [x0, x1) x [y0, y1) from another W x H buffer
void copyRectFrom(const uint16_t *src, int x0, int y0, int x1, int y1);
// same, only the pixels whose centre is within radius r of (cx, cy)
void copyDiscFrom(const uint16_t *src, int x0, int y0, int x1, int y1, float cx, float cy, float r);
void fillRect(int x, int y, int w, int h, uint16_t c);
void fillRectF(float x0, float y0, float x1, float y1, uint16_t c);  // smooth edges at fractional positions

enum : uint8_t { CORNER_TL = 1, CORNER_TR = 2, CORNER_BR = 4, CORNER_BL = 8, CORNER_ALL = 15 };
void fillRoundRect(float x, float y, float w, float h, float r, uint16_t c, uint8_t corners = CORNER_ALL);
void fillCircle(float cx, float cy, float r, uint16_t c);
// circle whose edge pixels mix c with base (not with what is on the screen)
void fillCircleOn(float cx, float cy, float r, uint16_t c, uint16_t base);
// round rect with a border inside the box (CSS border-box)
void frameRoundRect(float x, float y, float w, float h, float r, float border, uint16_t borderColor, uint16_t insideColor);
// annular sector, centre (cx, cy), radius rIn..rOut, angle a0..a1 [deg] (0 = 3 o'clock, clockwise;
// a1 - a0 >= 360: full ring). Smooth edges (edge pixels mix with what is there).
void fillArc(float cx, float cy, float rIn, float rOut, float a0, float a1, uint16_t c);
// same shape without smoothing (pixel centres inside); call with slightly larger radii/angles to erase fillArc
void fillArcHard(float cx, float cy, float rIn, float rOut, float a0, float a1, uint16_t c);
// glow: radius r0..r0+w, angle a0..a1, opacity alpha at r0 falling linearly to 0 at r0+w
void glowArc(float cx, float cy, float r0, float w, float a0, float a1, uint16_t c, uint8_t alpha);
// repaint a ring section: pixels of the ring rIn..rOut within a0..a1 become lit inside litA0..litA1 and
// track elsewhere; radial edges mix with base (round gauge 2.5.0)
void ringPaint(float cx, float cy, float rIn, float rOut, float a0, float a1, float litA0, float litA1, uint16_t lit, uint16_t track, uint16_t base);

// ---- polygons (smooth edges) ----
// polyBegin -> polyAdd (any number; overlaps count once) -> polyFill or polyToMask
void polyBegin(Rect bounds);
void polyAdd(const Pt *p, int n);
void polyFill(uint16_t c);
void polyToMask(uint8_t *mask);  // bounds width x height, 0..255 (no clip)
// pixel = base mixed with c by the mask; mask 0 = untouched. mirrorX mirrors at the screen centre.
void maskReplace(const uint8_t *mask, Rect r, bool mirrorX, uint16_t c, uint16_t base);
// helpers: one polygon; a thick line with square ends (width w)
void fillPoly(const Pt *p, int n, uint16_t c);
void fillQuad(float x0, float y0, float x1, float y1, float w, uint16_t c);
Rect ptsRect(const Pt *p, int n);  // box around points, 2 px margin, inside the screen

// ---- text and icons ----
void setTabularDigits(bool tabular);  // kept from the round gauge (all fonts here are tabular)
void setTracking(float px);           // extra advance after every glyph (letter spacing), default 0
float textWidth(const FontData &f, const char *utf8);
bool hasGlyphs(const FontData &f, const char *utf8);
bool hasGlyph(const FontData &f, uint32_t code);
float glyphAdvance(const FontData &f, uint32_t code);  // with tracking; missing = box width
// returns the next pen position. A missing glyph is drawn as a box outline and counted.
float drawText(const FontData &f, float x, float baseline, const char *utf8, uint16_t c);
float drawGlyph(const FontData &f, float x, float baseline, uint32_t code, uint16_t c);
void drawIcon(const IconData &ic, int x, int y, uint16_t c);
uint32_t missingGlyphs();       // glyphs that were not in the font data (0 = all good)
uint32_t nextCode(const char *&s);  // UTF-8 decoder (0 at the end)

// ---- dirty rectangles ----
void addDirty(int x0, int y0, int x1, int y1);
int dirtyCount();
Rect dirtyAt(int i);
void dirtyClear();
uint32_t dirtyPixels();

}  // namespace gfx
}  // namespace ui
