// ui/ui_int.h - shared internals of the UI (colours, text helpers, view state, scene redraw support).
// Not part of the public API (lt_ui.h does not include it).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "fonts_data.h"
#include "gfx.h"
#include "ui.h"

namespace ui {
namespace in {

using gfx::rgb;

// ---------------------------------------------------------------- colours (UI設計 §8)
constexpr uint16_t C_BG = rgb(0x000000);
constexpr uint16_t C_WHITE = rgb(0xFFFFFF);
constexpr uint16_t C_SUB = rgb(0xC8C8C8);    // secondary text
constexpr uint16_t C_LABEL = rgb(0x9A9A9A);  // labels
constexpr uint16_t C_YELLOW = rgb(0xFFD400);
constexpr uint16_t C_ORANGE = rgb(0xFF9500);
constexpr uint16_t C_GREEN = rgb(0x2ECC40);
constexpr uint16_t C_RED = rgb(0xFF1E2D);
constexpr uint16_t C_BLUE = rgb(0x4FA3FF);
constexpr uint16_t C_OFF = rgb(0x262626);    // unlit, separators, value badges
constexpr uint16_t C_PANEL = rgb(0x141414);
constexpr uint16_t C_DOT = rgb(0x4A4A4A);    // inactive dots and pills (canvas)
constexpr uint16_t C_TICK = rgb(0x6A6A6A);   // thin dial ticks (canvas)
constexpr uint16_t C_LINE = rgb(0x1E1E1E);   // row separators (canvas)
constexpr uint16_t C_RING_GREEN_DK = rgb(0x0E2414);
constexpr uint16_t C_RING_YELLOW_DK = rgb(0x2A2400);
constexpr uint16_t C_RING_RED_DK = rgb(0x2E0B0B);
constexpr uint16_t C_TINT_YELLOW = rgb(0x1F1A00);
constexpr uint16_t C_TINT_ORANGE = rgb(0x211300);
constexpr uint16_t C_TINT_BLUE = rgb(0x0D1A2A);
constexpr uint16_t C_TINT_GREY = rgb(0x1A1A1A);
constexpr uint16_t C_TINT_RED = rgb(0x200808);
constexpr uint16_t C_TINT_GREEN = rgb(0x08200E);  // VS BEST frame fill when faster (canvas)
constexpr uint16_t C_TINT_RED2 = rgb(0x2A0709);   // VS BEST frame fill when slower (canvas)
constexpr uint16_t C_RED_TEXT = rgb(0xFF6B74);    // pending checklist item text (canvas)
constexpr uint16_t C_FLASH_DIM = rgb(0x590A10);   // shift flash red at 35 % (night)
constexpr uint16_t C_WHITE_DIM = rgb(0x595959);   // white at 35 % (night flash ring)

uint16_t dim35(uint16_t c);
uint16_t statusColor(uint8_t st);  // Status -> dot colour

// ---------------------------------------------------------------- text
struct TextSet {
  const FontData *lat;  // Latin / digits
  const FontData *jp;   // everything the Latin font does not have
};
extern const TextSet T13, T14, T18, T22, T26, T28, T30;
float mixWidth(const TextSet &t, const char *s);
float mixDraw(const TextSet &t, float x, float base, const char *s, uint16_t c);
float mixDrawR(const TextSet &t, float xr, float base, const char *s, uint16_t c);  // right aligned
float mixDrawC(const TextSet &t, float cx, float base, const char *s, uint16_t c);  // centred
// draw s at most maxW wide: with the first set that fits, else with the last one cut with "…"
float mixDrawFit(const TextSet *sets, int nSets, float x, float base, float maxW, const char *s, uint16_t c);
float textC(const FontData &f, float cx, float base, const char *s, uint16_t c);
float textR(const FontData &f, float xr, float base, const char *s, uint16_t c);
// letter-spaced label (tracking in em)
float labelW(const FontData &f, const char *s, float em);
float label(const FontData &f, float x, float base, const char *s, uint16_t c, float em);

// ---------------------------------------------------------------- shapes
// parallelogram: bottom edge x..x+w at y+h, top edge shifted right by slant
void fillPara(float x, float y, float w, float h, float slant, uint16_t c);
void framePara(float x, float y, float w, float h, float slant, float border, uint16_t cBorder, uint16_t cFill);
void checkMark(float cx, float cy, float s, uint16_t c);  // ✓ in a box of size s
void crossMark(float cx, float cy, float s, uint16_t c);  // ✗
// triangle with a black edge: tip, and the base centre; base width bw (the edge is outside by e)
void edgedTriangle(gfx::Pt tip, gfx::Pt baseC, float bw, float e, uint16_t fill, uint16_t edge);
gfx::Rect triangleRect(gfx::Pt tip, gfx::Pt baseC, float bw, float e);
// pill: rounded rect with a dot and a label (13 px)
void pill(float x, float y, float w, float h, const char *text, uint16_t dot, uint16_t fg);
// rounded badge with centred text
void badge(float x, float y, float w, float h, float r, const TextSet &t, const char *s, uint16_t bg, uint16_t fg);
// button: outlined (fill = C_BG) or filled
void button(float x, float y, float w, float h, const TextSet &t, const char *s, uint16_t border, uint16_t fill, uint16_t fg);

// ---------------------------------------------------------------- page header (§7)
struct PageStyle {
  uint16_t color, tint;
  const IconData *icon, *icon2;
  const char *name;
};
const PageStyle &pageStyle(uint8_t screen);
constexpr int kHeaderH = 64;  // band 64 px (+2 px line)
constexpr int kBodyY = 66;
constexpr int kRowH = 76;
void drawHeader(uint8_t screen, const char *aux);  // inside the current clip
int pageIndex(uint8_t screen);                     // 0 drive .. 4 settings, -1 other

// ---------------------------------------------------------------- formatting (integers only)
void fmtLap(char *o, size_t n, uint32_t ms);           // m:ss.cc
void fmtSec2(char *o, size_t n, uint32_t ms);          // ss.cc or m:ss.cc
void fmtDelta(char *o, size_t n, float s, bool plusMinus);  // −0.57 / +0.57
void fmtGrouped(char *o, size_t n, uint32_t v);        // 48,215
void fmtTenths(char *o, size_t n, float v);            // 0.4
void fmtDate(char *o, size_t n, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi);  // 11/22 10:32

// ---------------------------------------------------------------- small helpers
uint32_t hashBytes(uint32_t h, const void *p, size_t n);
uint32_t hashStr(uint32_t h, const char *s);
inline uint32_t hashU(uint32_t h, uint32_t v) {
  return hashBytes(h, &v, sizeof v);
}
constexpr uint32_t kHash0 = 2166136261u;
bool overlap(const gfx::Rect &a, const gfx::Rect &b);
gfx::Rect rectXY(int x0, int y0, int x1, int y1);
gfx::Rect unite(const gfx::Rect &a, const gfx::Rect &b);
bool emptyRect(const gfx::Rect &r);
float clampf(float v, float lo, float hi);

// list of rectangles to redraw, merged when that adds few pixels
struct RectList {
  static constexpr int kMax = 40;
  int n = 0;
  gfx::Rect r[kMax];
  void clear() { n = 0; }
  void add(const gfx::Rect &a, int slack = 900);
};

// ---------------------------------------------------------------- view state
enum TouchMode : uint8_t { TM_IDLE = 0, TM_PENDING, TM_HSWIPE, TM_VSCROLL, TM_DEAD };
struct State {
  uint16_t *layer = nullptr;
  bool full = true;
  uint8_t shown = 0xFF;  // screen drawn last
  uint8_t shownDrive = 0xFF;
  // shift flash timing
  bool flashActive = false;
  uint32_t flashStart = 0;
  bool flashRed = false;
  uint32_t nextDue = 0;
  // page view state (reset when the screen changes)
  int16_t scroll = 0;
  int8_t lapOpen = -1;
  int8_t maintDialog = -1;
  int8_t editor = -1;
  int8_t shiftSel = 0;
  int16_t gatePick = -1;
  // touch
  uint8_t mode = TM_IDLE;
  int16_t x0 = 0, y0 = 0, lx = 0, ly = 0, maxMove = 0, scroll0 = 0;
  uint32_t t0 = 0;
};
extern State g;
void resetView();
bool flashPhase(const Model &m, uint32_t now);  // true = red phase (also updates g.nextDue)
uint16_t flashBg(const Model &m);                // background colour of the red phase

// ---------------------------------------------------------------- screens
void selftestFrame(const Model &m, bool full);
void circuitFrame(const Model &m, uint32_t now, bool full);
void streetFrame(const Model &m, uint32_t now, bool full);
void pagesFrame(const Model &m, bool full);  // SC_LAPS .. SC_GATE
// pages: content height of the scrolled body (for clamping), tap handling
int pageScrollMax(const Model &m);
void pageTap(const Model &m, int x, int y, ActionQueue &out);
bool pageModal();  // a dialog or editor takes the taps (no swipe)

}  // namespace in
}  // namespace ui
