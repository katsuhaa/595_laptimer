// ui/ui.h - the screens of the 595_laptimer (UI設計.md v0.9 §3-§7), drawn with the round gauge's renderer (gfx).
//
// Use:
//   allocate fb (kFrameBytes), scratch (kScratchBytes) and layer (kLayerBytes) once, e.g. with ps_malloc, then
//   ui::begin(fb, scratch, layer);
//   loop: fill the Model; for each touch event ui::touch(in, now, model, actions); apply the actions to the
//         model (e.g. ACT_PAGE -> model.screen); ui::frame(model, now, &dirty); push dirty.r[0..n) to the panel.
// frame() draws only what changed since the previous frame. Call it at 25 Hz or more on the drive screen
// (ring 25 Hz, hands 20 Hz, shift flash 6 Hz with 83 ms phases timed from nowMs).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "ui_model.h"

namespace ui {

constexpr size_t kFrameBytes = (size_t)kWidth * kHeight * 2;          // RGB565 frame buffer
constexpr size_t kScratchBytes = (size_t)kWidth * (kHeight + 2) * 4;  // polygon scratch (floats)
constexpr size_t kLayerBytes = (size_t)kWidth * kHeight * 2;          // cached static layer of the circuit screen

// framebuffer: kFrameBytes, scratch: kScratchBytes, layer: kLayerBytes or nullptr (then the circuit screen
// redraws its dial under moving hands instead of copying it: same pixels, slower). All from the caller (PSRAM).
void begin(uint16_t *framebuffer, float *scratch, uint16_t *layer = nullptr);

// Touch events. Ignored while model.touchEnabled is false (a gesture in progress is dropped).
// Horizontal swipe >= 50 px: ACT_PAGE to the next / previous page (stops at the ends); vertical drag scrolls rows;
// a tap (< 24 px movement, >= 60 ms) acts on what is under it. Actions are appended to out.
void touch(const Input &in, uint32_t nowMs, const Model &m, ActionQueue &out);

// Draw what changed into the frame buffer. out (may be nullptr) receives the changed rectangles of this call.
// Returns the number of rectangles.
int frame(const Model &m, uint32_t nowMs, Dirty *out);

// Redraw everything on the next frame (e.g. after the panel was off).
void invalidate();

// Time of the next change that frame() must draw without a model change (flash phase edge), 0 = none.
uint32_t nextDueMs();

uint32_t missingGlyphs();  // characters drawn as a box because they are not in the font data (0 = good)

// Copy UTF-8 text into a fixed-size model string; cuts at a character boundary. Returns dst.
char *copyText(char *dst, size_t size, const char *src);

// ---- for tests (host render tool)
struct ViewState {
  uint8_t screen;
  int16_t scroll;       // body scroll of the current page [px]
  int8_t lapOpen;       // laps page: row with sectors shown, -1 none
  int8_t maintDialog;   // maint page: item index of the open dialog, -1 none
  int8_t editor;        // settings page: open editor (SettingsRow), -1 none
  int8_t shiftSel;      // shift editor: selected value 0..3
  int16_t gatePick;     // gate page: picked track point, -1 none
  bool flashRed;        // last drawn flash phase
};
ViewState viewState();
void testSetView(const ViewState &v);  // apply the view state fields (screen must match model.screen)

}  // namespace ui
