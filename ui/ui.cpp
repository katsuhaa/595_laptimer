// ui/ui.cpp - entry points: begin, touch (swipe / scroll / tap), frame (dispatch to the screens), view state
#include "ui.h"

#include <string.h>

#include "ui_int.h"

namespace ui {

namespace in {

State g;

void resetView() {
  g.scroll = -1;  // set by the page on its first frame (laps: the latest laps at the bottom)
  g.lapOpen = -1;
  g.maintDialog = -1;
  g.editor = -1;
  g.shiftSel = 0;
  g.gatePick = -1;
  g.mode = TM_IDLE;
}

// Shift flash: 6 Hz, 83 ms red then 83 ms black, counted from the moment the app set model.flash (starts red)
static const uint32_t kFlashHalfMs = 83;
bool flashPhase(const Model &m, uint32_t now) {
  if (!m.flash) {
    g.flashActive = false;
    return false;
  }
  if (!g.flashActive) {
    g.flashActive = true;
    g.flashStart = now;
  }
  const uint32_t t = now - g.flashStart;
  const uint32_t k = t / kFlashHalfMs;
  const uint32_t next = g.flashStart + (k + 1) * kFlashHalfMs;
  if (g.nextDue == 0 || (int32_t)(next - g.nextDue) < 0) g.nextDue = next ? next : 1;
  return (k & 1u) == 0;
}
uint16_t flashBg(const Model &m) {
  return m.nightDim ? C_FLASH_DIM : C_RED;
}

}  // namespace in

using namespace in;

void begin(uint16_t *framebuffer, float *scratch, uint16_t *layer) {
  gfx::begin(framebuffer, scratch);
  g = State();
  g.layer = layer;
  g.full = true;
  resetView();
}

void invalidate() {
  g.full = true;
}

uint32_t nextDueMs() {
  return g.nextDue;
}

uint32_t missingGlyphs() {
  return gfx::missingGlyphs();
}

char *copyText(char *dst, size_t size, const char *src) {
  if (!dst || !size) return dst;
  size_t n = 0;
  if (src) {
    while (src[n]) {
      const uint8_t c = (uint8_t)src[n];
      const size_t len = c < 0x80 ? 1 : (c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : 2));
      if (n + len >= size) break;  // the whole character and the NUL must fit
      n += len;
    }
    memcpy(dst, src, n);
  }
  dst[n] = 0;
  return dst;
}

// ---------------------------------------------------------------- touch
static const int kTapSlop = 24;    // a tap moves less than this [px]
static const int kSwipeMin = 50;   // horizontal swipe threshold [px]
static const uint32_t kTapMinMs = 60;  // shorter touches do nothing (brushes, vibration)

static bool scrollable(uint8_t screen) {
  return screen == SC_LAPS || screen == SC_MAINT || screen == SC_DATA || screen == SC_SETTINGS;
}
static uint8_t pageAt(int idx) {
  static const uint8_t order[5] = { SC_DRIVE, SC_LAPS, SC_MAINT, SC_DATA, SC_SETTINGS };
  return order[idx];
}
static inline int iabs(int v) {
  return v < 0 ? -v : v;
}

void touch(const Input &inp, uint32_t nowMs, const Model &m, ActionQueue &out) {
  (void)nowMs;
  if (!m.touchEnabled) {
    g.mode = TM_IDLE;
    return;
  }
  if (inp.kind == IN_DOWN) {
    g.mode = TM_PENDING;
    g.x0 = g.lx = inp.x;
    g.y0 = g.ly = inp.y;
    g.t0 = inp.ms;
    g.maxMove = 0;
    g.scroll0 = g.scroll < 0 ? 0 : g.scroll;
    return;
  }
  if (g.mode == TM_IDLE) return;
  g.lx = inp.x;
  g.ly = inp.y;
  const int dx = g.lx - g.x0, dy = g.ly - g.y0;
  const int mv = iabs(dx) > iabs(dy) ? iabs(dx) : iabs(dy);
  if (mv > g.maxMove) g.maxMove = (int16_t)mv;
  if (g.mode == TM_PENDING && mv >= kTapSlop) {
    if (iabs(dy) > iabs(dx) && scrollable(m.screen) && !pageModal() && m.screen == g.shown) g.mode = TM_VSCROLL;
    else if (iabs(dx) > iabs(dy)) g.mode = TM_HSWIPE;
    else g.mode = TM_DEAD;
  }
  if (g.mode == TM_VSCROLL) {
    int s = g.scroll0 - dy;
    const int mx = pageScrollMax(m);
    if (s > mx) s = mx;
    if (s < 0) s = 0;
    g.scroll = (int16_t)s;
  }
  if (inp.kind != IN_UP) return;
  const uint8_t mode = g.mode;
  g.mode = TM_IDLE;
  if (mode == TM_HSWIPE) {
    if (iabs(dx) < kSwipeMin || iabs(dx) * 2 < iabs(dy) * 3) return;  // too short or too steep
    if (m.screen == SC_SELFTEST || m.screen == SC_GATE || pageModal()) return;
    const int idx = pageIndex(m.screen);
    if (idx < 0) return;
    const int to = dx < 0 ? idx + 1 : idx - 1;  // finger to the left = next page
    if (to < 0 || to > 4) return;               // stop at the ends
    Action a;
    a.kind = ACT_PAGE;
    a.arg = pageAt(to);
    out.push(a);
    return;
  }
  if (mode != TM_PENDING || g.maxMove >= kTapSlop || (uint32_t)(inp.ms - g.t0) < kTapMinMs) return;
  if (m.screen != g.shown) return;  // the screen the finger saw is not drawn yet
  if (m.screen == SC_SELFTEST) {
    Action a;
    a.kind = ACT_SELFTEST_SKIP;
    out.push(a);
    return;
  }
  if (m.screen == SC_DRIVE) return;  // nothing to tap while driving
  pageTap(m, g.x0, g.y0, out);
}

// ---------------------------------------------------------------- frame
int frame(const Model &m, uint32_t nowMs, Dirty *out) {
  gfx::dirtyClear();
  gfx::clearClip();
  gfx::setAlpha(255);
  gfx::setTarget(nullptr);
  bool full = g.full;
  if (m.screen != g.shown) {
    resetView();
    full = true;
  }
  if (m.screen == SC_DRIVE && m.drive != g.shownDrive) full = true;
  g.full = false;
  g.nextDue = 0;
  if (m.screen != SC_DRIVE) g.flashActive = false;
  if (scrollable(m.screen) && g.scroll >= 0) {
    const int mx = pageScrollMax(m);
    if (g.scroll > mx) g.scroll = (int16_t)mx;
    if (g.scroll < 0) g.scroll = 0;
  }
  switch (m.screen) {
    case SC_SELFTEST: selftestFrame(m, full); break;
    case SC_DRIVE:
      if (m.drive == DRIVE_CIRCUIT) circuitFrame(m, nowMs, full);
      else streetFrame(m, nowMs, full);
      break;
    default: pagesFrame(m, full); break;
  }
  g.shown = m.screen;
  g.shownDrive = m.drive;
  gfx::clearClip();
  gfx::setTarget(nullptr);
  const int n = gfx::dirtyCount();
  if (out) {
    out->n = 0;
    for (int i = 0; i < n && i < Dirty::kMax; i++) {
      gfx::Rect r = gfx::dirtyAt(i);
      out->r[out->n++] = Rect{ r.x0, r.y0, r.x1, r.y1 };
    }
  }
  return n;
}

// ---------------------------------------------------------------- tests
ViewState viewState() {
  ViewState v;
  v.screen = g.shown;
  v.scroll = g.scroll;
  v.lapOpen = g.lapOpen;
  v.maintDialog = g.maintDialog;
  v.editor = g.editor;
  v.shiftSel = g.shiftSel;
  v.gatePick = g.gatePick;
  v.flashRed = g.flashRed;
  return v;
}
void testSetView(const ViewState &v) {
  g.scroll = v.scroll;
  g.lapOpen = v.lapOpen;
  g.maintDialog = v.maintDialog;
  g.editor = v.editor;
  g.shiftSel = v.shiftSel;
  g.gatePick = v.gatePick;
  g.full = true;
}

}  // namespace ui
