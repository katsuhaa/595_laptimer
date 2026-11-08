#!/usr/bin/env python3
# gen_assets.py - fonts and icons of the 595_laptimer UI -> ui/fonts_data.h, ui/fonts_data.cpp
#
# Port of the round gauge's pc_test/gen_assets.py. Same glyph format: 4-bit coverage per pixel, folded with
# RLE (1 byte = (run-1) << 4 | value; run-1 == 15 means: run = 16 + next byte).
# Fonts (SIL OFL 1.1, downloaded by fetch_fonts.sh):
#   Barlow Condensed Bold Italic - digits and ":.,-+%−/" (tabular figures)
#   Chakra Petch Bold / SemiBold - Latin labels (ASCII and a few symbols)
#   IBM Plex Sans JP Bold / SemiBold - Japanese
# Japanese characters:
#   UI_JP     every non-ASCII character in the string literals of ui/*.cpp and of the app that fills the model
#             (firmware/laptimer/src/app/*.cpp: self-test values, remaining text ...; comments are skipped), read
#             each run, so a new word in the UI or the app cannot be forgotten
#   APP_TEXT  words that the app writes into ui::Model (self-test values, maintenance remaining text ...)
#   JP_BROAD  kana and common kanji for names that come from the configuration (maintenance items, course names,
#             Wi-Fi names); only at the sizes the UI uses for such names (18, 22, 28 px)
# A character that is missing in the data is drawn as a box and counted (ui::missingGlyphs()).
#
# Usage: python3 -I tools/assets/gen_assets.py <fonts dir> <output dir>      (normally: build/fonts ui)
# Needs: pip3 install freetype-py uharfbuzz numpy pillow
import pathlib
import re
import sys

import freetype
import numpy as np
import uharfbuzz as hb
from PIL import Image, ImageDraw

FONTS = pathlib.Path(sys.argv[1])
OUT = pathlib.Path(sys.argv[2])
ROOT = pathlib.Path(__file__).resolve().parent.parent.parent

BAR = "BarlowCondensed-BoldItalic.ttf"
CPB = "ChakraPetch-Bold.ttf"
CPS = "ChakraPetch-SemiBold.ttf"
JPB = "IBMPlexSansJP-Bold.ttf"
JPS = "IBMPlexSansJP-SemiBold.ttf"

ASCII = "".join(chr(c) for c in range(0x20, 0x7F))
DIGITS = "0123456789:.,-+%−/ "
LATIN_EXTRA = "°·›‹…−→×±"  # in Chakra Petch


def uniq(*texts):
    return "".join(dict.fromkeys(ch for t in texts for ch in t))


def jp_only(*texts):  # the non-ASCII characters
    return "".join(dict.fromkeys(ch for t in texts for ch in t if ord(ch) >= 0x80))


# ---- characters in the UI and app sources (string literals, comments skipped)
LITERAL_DIRS = ["ui", "firmware/laptimer/src/app"]


def ui_literal_chars():
    out = []
    for path in [p for d in LITERAL_DIRS for p in sorted((ROOT / d).glob("*.cpp"))]:
        if path.name == "fonts_data.cpp":
            continue
        src = path.read_text(encoding="utf-8")
        i, n = 0, len(src)
        while i < n:
            c = src[i]
            if src.startswith("//", i):
                j = src.find("\n", i)
                i = n if j < 0 else j
            elif src.startswith("/*", i):
                j = src.find("*/", i + 2)
                i = n if j < 0 else j + 2
            elif c == "'":  # character literal
                j = i + 1
                while j < n and src[j] != "'":
                    j += 2 if src[j] == "\\" else 1
                i = j + 1
            elif c == '"':
                j = i + 1
                while j < n and src[j] != '"':
                    j += 2 if src[j] == "\\" else 1
                out += [ch for ch in src[i + 1:j] if ord(ch) >= 0x80]
                i = j + 1
            else:
                i += 1
    return "".join(dict.fromkeys(out))


UI_JP = ui_literal_chars()

APP_TEXT = jp_only(
    # self-test values (written by the app)
    "衛星 空き 書込 読戻し 一致 不一致 フレーム エラー 応答 応答なし 購読 待機 接続 接続中 未接続 失敗 不明 正常 異常 件 確認中 待ち 測位 時刻 電圧 低い 高い なし 秒 分 時間 記録 停止 再生",
    "（）・→●○ 共有 未送信 未ダウンロード ダウンロード済み",
    # maintenance remaining text and course notes
    "残り 期限超過 期限 超過 日 ヶ月 か月 年 週 時間 サーキット 走行 まで 徐行中 停止中 約",
)

HIRA = "".join(chr(c) for c in range(0x3041, 0x3097))
KATA = "".join(chr(c) for c in range(0x30A1, 0x30FB)) + "ー"
JP_PUNCT = "、。・「」『』（）［］〜～：；！？％＃＆＊＋－／＝｜…‥ー々〆〇→←↑↓●○◎■□▲△▼▽"
KANJI_G1 = "一右雨円王音下火花貝学気九休玉金空月犬見五口校左三山子四糸字耳七車手十出女小上森人水正生青夕石赤千川先早草足村大男竹中虫町天田土二日入年白八百文木本名目立力林六"
KANJI_G2 = (
    "引羽雲園遠何科夏家歌画回会海絵外角楽活間丸岩顔汽記帰弓牛魚京強教近兄形計元言原戸古午後語工公広交光考行高黄合谷国黒今才細作算止市矢姉思紙寺自時室社弱首秋週春書少場色食心新親図数西声星晴切雪船線前組走多太体台地池知茶昼長鳥朝直通弟店点電刀冬当東答頭同道読内南肉馬売買麦半番父風分聞米歩母方北毎妹万明鳴毛門夜野友用曜来里理話"
)
KANJI_CAR = (  # maintenance, parts, measurements
    "交換点検整備替補充済予定季節燃費距離冷却液剤圧漏摩耗劣化錆洗塗装磨削溝泥輪軸駆制御操舵踏筒弁栓管継帯鎖座枠鋼銅鉛池灯端配蓄媒湿乾燥濾過器噴射排吸縮芯"
    "警告確認設計測記録再停止接続信号受送衛星位置精度速温量残超期限最低均差純社外強化品部材料類種別型式両検査証保険税"
    "油整備館研究整調子板皮重軽荷物発進転動流転旧次初終始全割合問題番対応待機失敗不良異常注意危険安"
)
KANJI_PLACE = (  # circuits, prefectures, cities
    "袖浦筑波富士鈴鹿茂原菅生幸桶瀬那須群馬栃埼玉千葉茨城宮福島形潟梨静岡阜愛知滋奈良兵庫鳥取根徳香媛佐賀崎熊縄沖広岩秋青森北海道仙台札幌函館浜松豊橋古屋阪神戸姫路倉敷関門博多児本分府金沢富山井京都和歌津"
    "茂木美備勝十日光庄間浅幸国際九州近畿四沼湖峠坂谷岬港湾浦塚橋寺宿駅"
)
KANJI_NAMES = (  # common family names (Wi-Fi names like "名前のiPhone")
    "佐藤鈴渡辺伊吉松清崎阿池橋石岡後谷坂遠西福太浦原野竹内金子和柴酒工宮河武杉増塚菅久保千桜菊尾新部斉斎齋齊加藤平今岸根克治藤田村林森"
)
JP_BROAD = uniq(HIRA, KATA, JP_PUNCT, KANJI_G1, KANJI_G2, KANJI_CAR, KANJI_PLACE, KANJI_NAMES)

# 14 px Japanese: the Wi-Fi hint (ui/scr_pages.cpp WIFI_HINT, under the scan list and the "見つかりません" result).
# Only these words are drawn at 14 px; a word added there must be added here (render counts missing glyphs).
WIFI_HINT_14 = "2.4 GHz のネットワークだけ表示されます（iPhone のインターネット共有は「互換性を優先」をオン）"


def spec(name, px, parts, ref):
    return dict(name=name, px=px, parts=parts, ref=ref)


def num(px):
    return spec("NUM%d" % px, px, [dict(file=BAR, feats={"tnum": True}, chars=DIGITS)], "0")


def cp(px, bold):
    return spec("CP%d" % px, px, [dict(file=CPB if bold else CPS, chars=ASCII + LATIN_EXTRA)], "H")


def jp(px, bold, chars):
    return spec("JP%d" % px, px, [dict(file=JPB if bold else JPS, chars=jp_only(chars))], "日")


JP_ALL = uniq(UI_JP, APP_TEXT)
SPECS = [
    num(88),  # circuit: peak rpm in the dial centre
    num(64),  # street: tile values
    num(48),  # circuit: VS BEST delta; settings editor values
    num(46),  # laps page: lap time
    num(44),  # circuit: lap time under the ring
    num(34),  # circuit: LAP, LAST, BEST; street: rpm
    num(28),  # circuit: dial numbers; laps page: difference, sectors
    num(24),  # circuit: clock numbers 12 / 3 / 6 / 9
    num(20),  # street: clock in the status row
    num(16),  # circuit: digital time under the clock
    cp(13, True),   # tags, pills, small labels (letter-spaced)
    cp(14, False),  # circuit: speed / GPS line
    cp(18, False),  # 18 px Latin next to 18 px Japanese
    cp(22, True),
    cp(24, True),
    cp(26, True),
    cp(28, True),
    cp(30, True),
    jp(13, False, "油温・"),  # circuit: oil pill
    jp(14, False, uniq("・", WIFI_HINT_14)),  # circuit: speed / GPS line; Wi-Fi hint
    jp(18, False, uniq(JP_ALL, JP_BROAD)),  # secondary text, course names, Wi-Fi names
    jp(22, False, uniq(JP_ALL, JP_BROAD)),  # values, badges, session cards, self-test values
    jp(26, True, JP_ALL),
    jp(28, True, uniq(JP_ALL, JP_BROAD)),   # row names (maintenance items, settings)
    jp(30, True, JP_ALL),                   # page names
]

_cache = {}


def load(file):
    if file not in _cache:
        path = str(FONTS / file)
        blob = hb.Blob.from_file_path(path)
        face = hb.Face(blob)
        _cache[file] = (path, face)
    return _cache[file]


def rle4(vals):
    out = bytearray()
    i = 0
    n = len(vals)
    while i < n:
        v = vals[i]
        j = i + 1
        while j < n and vals[j] == v and j - i < 271:
            j += 1
        run = j - i
        if run <= 15:
            out.append(((run - 1) << 4) | v)
        else:
            out.append(0xF0 | v)
            out.append(run - 16)
        i = j
    return bytes(out)


def render_part(part, px):
    path, face = load(part["file"])
    font = hb.Font(face)
    ft = freetype.Face(path)
    ft.set_pixel_sizes(0, px)
    glyphs = []
    for ch in part["chars"]:
        buf = hb.Buffer()
        buf.add_str(ch)
        buf.guess_segment_properties()
        hb.shape(font, buf, part.get("feats", {}))
        info, pos = buf.glyph_infos[0], buf.glyph_positions[0]
        if info.codepoint == 0 and ch != " ":
            raise SystemExit(f"glyph missing: U+{ord(ch):04X} {ch} in {part['file']}")
        adv = pos.x_advance * px / face.upem
        ft.load_glyph(info.codepoint, freetype.FT_LOAD_NO_HINTING | freetype.FT_LOAD_RENDER)
        bm = ft.glyph.bitmap
        w, h = bm.width, bm.rows
        a = np.array(bm.buffer, np.uint8).reshape(h, w) if w and h else np.zeros((0, 0), np.uint8)
        if w > 255 or h > 255:
            raise SystemExit("glyph too large")
        q = ((a.astype(np.uint16) * 15 + 127) // 255).astype(np.uint8)
        glyphs.append(dict(code=ord(ch), w=w, h=h, xoff=ft.glyph.bitmap_left, yoff=ft.glyph.bitmap_top,
                           adv=int(round(adv * 16)), data=rle4(q.flatten().tolist()), ch=ch))
    return glyphs


# ---- icons: 36 px, two layers (main shape in the page colour, a second tone drawn dimmer under it).
# Drawn with Pillow at 8x and reduced by area averaging (no browser needed).
S = 8


def _img():
    im = Image.new("L", (36 * S, 36 * S), 0)
    return im, ImageDraw.Draw(im)


def _P(*pts):
    return [(x * S, y * S) for x, y in pts]


def _line(d, pts, w):
    d.line(_P(*pts), fill=255, width=int(w * S), joint="curve")
    r = w * S / 2
    for x, y in (pts[0], pts[-1]):  # round caps
        d.ellipse([x * S - r, y * S - r, x * S + r, y * S + r], fill=255)


def _disc(d, cx, cy, r, v=255):
    d.ellipse([(cx - r) * S, (cy - r) * S, (cx + r) * S, (cy + r) * S], fill=v)


def _ring(d, cx, cy, r, w):
    _disc(d, cx, cy, r + w / 2)
    _disc(d, cx, cy, r - w / 2, 0)


def icon_stopwatch():
    im, d = _img()
    _ring(d, 18, 20.5, 12, 3)
    d.rounded_rectangle([14.5 * S, 2 * S, 21.5 * S, 5 * S], radius=1 * S, fill=255)  # crown
    d.rectangle([16.75 * S, 4 * S, 19.25 * S, 8.5 * S], fill=255)
    _line(d, [(27.5, 9.5), (29.5, 7.5)], 3)  # side button
    _line(d, [(18, 20.5), (18, 12.5)], 3)    # hands
    _line(d, [(18, 20.5), (23.5, 23.5)], 3)
    im2, d2 = _img()
    _disc(d2, 18, 20.5, 10.5)
    return im, im2


def icon_wrench():
    im, d = _img()
    _line(d, [(9, 27), (22.5, 13.5)], 6)  # handle
    _disc(d, 25, 11, 8)                   # head
    slot = [(25 + dx, 11 + dy) for dx, dy in ((-2.6, -2.6), (2.6, 2.6), (12, -6.8), (6.8, -12))]  # open jaw (towards top right)
    d.polygon(_P(*slot), fill=0)
    _disc(d, 25, 11, 3.2, 0)
    _disc(d, 9, 27, 1.6, 0)  # hole in the handle
    im2, d2 = _img()
    hexa = [(28 + 5 * np.cos(np.radians(30 + 60 * k)), 28 + 5 * np.sin(np.radians(30 + 60 * k))) for k in range(6)]
    d2.polygon(_P(*hexa), fill=255)  # nut
    _disc(d2, 28, 28, 2, 0)
    return im, im2


def icon_phone():
    im, d = _img()
    d.rounded_rectangle([9 * S, 2 * S, 27 * S, 34 * S], radius=4.5 * S, fill=255)
    d.rounded_rectangle([12 * S, 5 * S, 24 * S, 31 * S], radius=2 * S, fill=0)
    _line(d, [(18, 9.5), (18, 24.5)], 3)  # down arrow
    _line(d, [(13.2, 19.8), (18, 24.6), (22.8, 19.8)], 3)
    im2, d2 = _img()
    d2.rounded_rectangle([12 * S, 5 * S, 24 * S, 31 * S], radius=2 * S, fill=255)
    return im, im2


def icon_gear():
    im, d = _img()
    pts = []
    teeth = 8
    for k in range(teeth):
        a0 = 360.0 / teeth * k
        for da, r in ((-14, 11.6), (-9, 15.6), (9, 15.6), (14, 11.6)):
            a = np.radians(a0 + da)
            pts.append((18 + r * np.cos(a), 18 + r * np.sin(a)))
    d.polygon(_P(*pts), fill=255)
    _disc(d, 18, 18, 7.5, 0)
    _disc(d, 18, 18, 3.6)
    im2, d2 = _img()
    _disc(d2, 18, 18, 7.5)
    _disc(d2, 18, 18, 3.6, 0)
    return im, im2


def icon_layers():
    res = []
    for name, fn in (("LAPS", icon_stopwatch), ("MAINT", icon_wrench), ("DATA", icon_phone), ("SET", icon_gear)):
        a, b = fn()
        for suffix, im in (("", a), ("_2", b)):
            small = im.resize((36, 36), Image.BOX)
            q = ((np.array(small, np.uint16) * 15 + 127) // 255).astype(np.uint8)
            res.append(dict(name=name + suffix, w=36, h=36, data=rle4(q.flatten().tolist())))
    return res


def carray(data, per=24):
    lines = []
    for i in range(0, len(data), per):
        lines.append("  " + ",".join(str(b) for b in data[i:i + per]) + ",")
    return "\n".join(lines)


def main():
    h = ["// fonts_data.h - font and icon data of the 595_laptimer UI (made by tools/assets/gen_assets.py; do not edit)",
         "// Fonts: Barlow Condensed (The Barlow Project Authors), Chakra Petch (Cadson Demak), IBM Plex Sans JP (IBM Corp.),",
         "// all under the SIL Open Font License 1.1. This file holds glyph coverage rendered at fixed sizes.",
         "#pragma once", "#include <stdint.h>", "", "namespace ui {", "",
         "struct GlyphInfo {", "  uint16_t code;    // Unicode code point (BMP)", "  uint8_t w, h;     // bitmap size [px]",
         "  int16_t xoff;     // pen position to the left edge of the bitmap [px]", "  int16_t yoff;     // baseline to the top of the bitmap (up positive) [px]",
         "  uint16_t adv16;   // advance [1/16 px]", "  uint32_t offset;  // start in rle", "};",
         "struct FontData {", "  const GlyphInfo *glyphs;  // ascending code", "  uint16_t count;", "  uint16_t px;",
         "  int16_t capH;   // height of a reference glyph above the baseline (digits: '0', Latin: 'H', Japanese: a kanji) [px]",
         "  int16_t descH;  // deepest glyph below the baseline [px]", "  const uint8_t *rle;", "  const char *name;", "};",
         "struct IconData {", "  uint8_t w, h;", "  const uint8_t *rle;", "};",
         "#define FONT_PROP_DIGIT_BASE 0xE030  // proportional digits (round gauge); not generated here", ""]
    c = ['// fonts_data.cpp - font and icon data of the 595_laptimer UI (made by tools/assets/gen_assets.py; do not edit)',
         '#include "fonts_data.h"', "", "namespace ui {", ""]
    total = 0
    report = []
    for sp in SPECS:
        glyphs = {}
        for part in sp["parts"]:
            for g in render_part(part, sp["px"]):
                glyphs[g["code"]] = g
        gl = [glyphs[k] for k in sorted(glyphs)]
        ref = glyphs.get(ord(sp["ref"]))
        capH = ref["yoff"] if ref else sp["px"] * 7 // 10
        descH = max([g["h"] - g["yoff"] for g in gl if g["h"]] + [0])
        blob = bytearray()
        rows = []
        for g in gl:
            rows.append(f"  {{0x{g['code']:04X},{g['w']},{g['h']},{g['xoff']},{g['yoff']},{g['adv']},{len(blob)}}},")
            blob += g["data"]
        n = sp["name"]
        c.append(f"static const uint8_t RLE_{n}[] = {{\n{carray(blob)}\n}};")
        c.append(f"static const GlyphInfo GLY_{n}[] = {{\n" + "\n".join(rows) + "\n};")
        c.append(f'const FontData FONT_{n} = {{GLY_{n}, {len(gl)}, {sp["px"]}, {capH}, {descH}, RLE_{n}, "{n}"}};\n')
        h.append(f"extern const FontData FONT_{n};")
        size = len(blob) + 16 * len(gl)  # sizeof(GlyphInfo) = 16
        total += size
        report.append((n, len(gl), len(blob), size))
    h.append("")
    icon_total = 0
    for ic in icon_layers():
        c.append(f"static const uint8_t RLE_ICON_{ic['name']}[] = {{\n{carray(ic['data'])}\n}};")
        c.append(f"const IconData ICON_{ic['name']} = {{{ic['w']}, {ic['h']}, RLE_ICON_{ic['name']}}};\n")
        h.append(f"extern const IconData ICON_{ic['name']};")
        icon_total += len(ic["data"])
    total += icon_total
    h += ["", "}  // namespace ui"]
    c += ["}  // namespace ui"]
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "fonts_data.h").write_text("\n".join(h) + "\n", encoding="utf-8")
    (OUT / "fonts_data.cpp").write_text("\n".join(c) + "\n", encoding="utf-8")
    for r in report:
        print("%-6s glyphs=%4d rle=%7d B  with glyph table=%7d B" % r)
    print("icons  %d B" % icon_total)
    print("UI Japanese characters: %d, app words: %d, broad set: %d" % (len(UI_JP), len(APP_TEXT), len(JP_BROAD)))
    print("TOTAL font+icon data: %d bytes (%.1f KiB) in flash" % (total, total / 1024.0))


main()
