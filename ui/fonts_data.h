// fonts_data.h - font and icon data of the 595_laptimer UI (made by tools/assets/gen_assets.py; do not edit)
// Fonts: Barlow Condensed (The Barlow Project Authors), Chakra Petch (Cadson Demak), IBM Plex Sans JP (IBM Corp.),
// all under the SIL Open Font License 1.1. This file holds glyph coverage rendered at fixed sizes.
#pragma once
#include <stdint.h>

namespace ui {

struct GlyphInfo {
  uint16_t code;    // Unicode code point (BMP)
  uint8_t w, h;     // bitmap size [px]
  int16_t xoff;     // pen position to the left edge of the bitmap [px]
  int16_t yoff;     // baseline to the top of the bitmap (up positive) [px]
  uint16_t adv16;   // advance [1/16 px]
  uint32_t offset;  // start in rle
};
struct FontData {
  const GlyphInfo *glyphs;  // ascending code
  uint16_t count;
  uint16_t px;
  int16_t capH;   // height of a reference glyph above the baseline (digits: '0', Latin: 'H', Japanese: a kanji) [px]
  int16_t descH;  // deepest glyph below the baseline [px]
  const uint8_t *rle;
  const char *name;
};
struct IconData {
  uint8_t w, h;
  const uint8_t *rle;
};
#define FONT_PROP_DIGIT_BASE 0xE030  // proportional digits (round gauge); not generated here

extern const FontData FONT_NUM88;
extern const FontData FONT_NUM64;
extern const FontData FONT_NUM48;
extern const FontData FONT_NUM46;
extern const FontData FONT_NUM44;
extern const FontData FONT_NUM34;
extern const FontData FONT_NUM28;
extern const FontData FONT_NUM24;
extern const FontData FONT_NUM20;
extern const FontData FONT_NUM16;
extern const FontData FONT_CP13;
extern const FontData FONT_CP14;
extern const FontData FONT_CP18;
extern const FontData FONT_CP22;
extern const FontData FONT_CP24;
extern const FontData FONT_CP26;
extern const FontData FONT_CP28;
extern const FontData FONT_CP30;
extern const FontData FONT_JP13;
extern const FontData FONT_JP14;
extern const FontData FONT_JP18;
extern const FontData FONT_JP22;
extern const FontData FONT_JP26;
extern const FontData FONT_JP28;
extern const FontData FONT_JP30;

extern const IconData ICON_LAPS;
extern const IconData ICON_LAPS_2;
extern const IconData ICON_MAINT;
extern const IconData ICON_MAINT_2;
extern const IconData ICON_DATA;
extern const IconData ICON_DATA_2;
extern const IconData ICON_SET;
extern const IconData ICON_SET_2;

}  // namespace ui
