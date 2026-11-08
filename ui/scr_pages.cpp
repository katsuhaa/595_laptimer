// ui/scr_pages.cpp - boot self-test (§3) and the stopped pages: laps, maintenance, data, settings, gate (§7)
// Each page is drawn as a header and a body; a part is redrawn (inside its clip) when its key changes.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_int.h"

namespace ui {
namespace in {

static const Model *s_m = nullptr;
static uint32_t s_keyHead = 0, s_keyBody = 0, s_keyRow[8], s_keyFoot = 0;
static const int kBodyH = gfx::H - kBodyY;  // 384

static void redraw(const gfx::Rect &r, void (*fn)()) {
  gfx::setClip(r);
  gfx::fillAll(C_BG);
  fn();
  gfx::clearClip();
}
static float baseC(const FontData &f, float cy) {  // baseline that centres capital letters / kanji at cy
  return cy + (float)f.capH * 0.5f;
}

// ================================================================ boot self-test
static const int ST_ROW_Y = 49, ST_ROW_H = 40, ST_FOOT_Y = 369;
static void stHeader() {
  const Model &m = *s_m;
  gfx::drawText(FONT_CP26, 16, baseC(FONT_CP26, 24), "SELF TEST", C_WHITE);
  char t[64];
  if (m.clock.ok)
    snprintf(t, sizeof t, "v%s ・ %04u-%02u-%02u %02u:%02u:%02u", m.fwVersion, (unsigned)m.clock.year, (unsigned)m.clock.month, (unsigned)m.clock.day,
             (unsigned)m.clock.hour, (unsigned)m.clock.minute, (unsigned)m.clock.second);
  else snprintf(t, sizeof t, "v%s ・ 時刻 待ち", m.fwVersion);
  mixDrawR(T18, 584, baseC(FONT_JP18, 24), t, C_LABEL);
  gfx::fillRect(0, 48, gfx::W, 1, C_OFF);
}
static int s_stRow = 0;
static void stRow() {
  const Model &m = *s_m;
  const int i = s_stRow;
  const float y = (float)(ST_ROW_Y + ST_ROW_H * i), cy = y + ST_ROW_H * 0.5f;
  if (i >= m.selftest.n) return;
  const SelfTestRow &r = m.selftest.row[i];
  const bool fail = r.result == TR_FAIL;
  if (fail) gfx::fillRect(0, (int)y, gfx::W, ST_ROW_H, C_TINT_RED);
  gfx::drawText(FONT_CP24, 16, baseC(FONT_CP24, cy), r.label, fail ? C_RED : C_WHITE);
  const TextSet sets[2] = { T22, T18 };
  mixDrawFit(sets, 2, 118, baseC(FONT_JP22, cy), 514 - 118 - 12, r.value, fail ? C_RED_TEXT : C_SUB);
  const uint16_t c = r.result == TR_PASS ? C_GREEN : (fail ? C_RED : C_LABEL);
  const char *s = r.result == TR_PASS ? "PASS" : (fail ? "FAIL" : "WAIT");
  gfx::frameRoundRect(514, cy - 15, 70, 30, 6, 2, c, fail ? C_TINT_RED : C_BG);
  textC(FONT_CP22, 549, baseC(FONT_CP22, cy), s, c);
}
static void stFoot() {
  const Model &m = *s_m;
  gfx::fillRect(0, ST_FOOT_Y, gfx::W, 1, C_OFF);
  // maintenance warnings: items that are due soon or overdue (the app sorts them)
  // whole items: two if they fit, else the first and "ほか n 件"
  int due = 0;
  for (int i = 0; i < m.maint.n; i++) due += m.maint.item[i].state != MS_OK;
  char w[200];
  for (int want = 2; want >= 1 && due; want--) {
    size_t used = 0;
    int shown = 0;
    for (int i = 0; i < m.maint.n && shown < want; i++) {
      const MaintItem &it = m.maint.item[i];
      if (it.state == MS_OK) continue;
      used += (size_t)snprintf(w + used, sizeof w - used, "%s%s %s", shown ? " ・ " : "整備: ", it.name, it.remain);
      if (used >= sizeof w) used = sizeof w - 1;
      shown++;
    }
    if (due > shown) snprintf(w + used, sizeof w - used, " ほか %d 件", due - shown);
    if (want == 1 || mixWidth(T18, w) <= 568) {
      const TextSet sets[1] = { T18 };
      mixDrawFit(sets, 1, 16, 397, 568, w, C_YELLOW);
      break;
    }
  }
  bool anyFail = false, anyWait = false;
  for (int i = 0; i < m.selftest.n; i++) {
    anyFail = anyFail || m.selftest.row[i].result == TR_FAIL;
    anyWait = anyWait || m.selftest.row[i].result == TR_WAIT;
  }
  char t[96];
  uint16_t c = C_WHITE;
  if (anyFail) {
    snprintf(t, sizeof t, "FAIL があります。タップで走行画面へ（記録は続けます）");
    c = C_RED;
  } else if (anyWait) {
    snprintf(t, sizeof t, "確認中 …（タップで走行画面へ）");
    c = C_SUB;
  } else if (m.selftest.advanceS >= 0) {
    snprintf(t, sizeof t, "すべて PASS → %d 秒後に走行画面へ（タップで今すぐ）", (int)m.selftest.advanceS);
  } else {
    snprintf(t, sizeof t, "すべて PASS（タップで走行画面へ）");
  }
  const TextSet sets[2] = { T22, T18 };
  mixDrawFit(sets, 2, 16, 432, 568, t, c);
}

void selftestFrame(const Model &m, bool full) {
  s_m = &m;
  if (full) {
    gfx::setClip(0, 0, gfx::W, gfx::H);
    gfx::fillAll(C_BG);
    gfx::clearClip();
  }
  uint32_t kh = hashStr(hashU(kHash0, m.clock.ok), m.fwVersion);
  kh = hashBytes(kh, &m.clock, sizeof m.clock);
  if (full || kh != s_keyHead) redraw(rectXY(0, 0, gfx::W, 49), stHeader);
  s_keyHead = kh;
  for (int i = 0; i < 8; i++) {
    uint32_t k = hashU(kHash0, (uint32_t)(i < m.selftest.n));
    if (i < m.selftest.n) k = hashU(hashStr(hashStr(k, m.selftest.row[i].label), m.selftest.row[i].value), m.selftest.row[i].result);
    if (full || k != s_keyRow[i]) {
      s_stRow = i;
      redraw(rectXY(0, ST_ROW_Y + ST_ROW_H * i, gfx::W, ST_ROW_Y + ST_ROW_H * (i + 1)), stRow);
    }
    s_keyRow[i] = k;
  }
  uint32_t kf = hashU(kHash0, (uint32_t)(int32_t)m.selftest.advanceS);
  for (int i = 0; i < m.selftest.n; i++) kf = hashU(kf, m.selftest.row[i].result);
  for (int i = 0; i < m.maint.n; i++) kf = hashU(hashStr(hashStr(kf, m.maint.item[i].name), m.maint.item[i].remain), m.maint.item[i].state);
  if (full || kf != s_keyFoot) redraw(rectXY(0, ST_FOOT_Y, gfx::W, gfx::H), stFoot);
  s_keyFoot = kf;
}

// ================================================================ laps
static const int LAP_SEC_H = 50;  // sector line under the opened row
static int lapTop(int i) {
  return i * kRowH + (g.lapOpen >= 0 && i > g.lapOpen ? LAP_SEC_H : 0);
}
static int lapsHeight(const Model &m) {
  return m.laps.n * kRowH + (g.lapOpen >= 0 && g.lapOpen < m.laps.n ? LAP_SEC_H : 0);
}
static int bestLap(const Model &m) {
  int b = -1;
  for (int i = 0; i < m.laps.n; i++)
    if (m.laps.lap[i].valid && (b < 0 || m.laps.lap[i].ms < m.laps.lap[b].ms)) b = i;
  return b;
}
static void lapsHeader() {
  const Model &m = *s_m;
  char aux[128], d[24];
  aux[0] = 0;
  if (m.laps.startOk) fmtDate(d, sizeof d, m.laps.month, m.laps.day, m.laps.hour, m.laps.minute);
  else d[0] = 0;
  snprintf(aux, sizeof aux, "%s%s%s%s%u 周", m.laps.course, m.laps.course[0] ? " " : "", d, d[0] ? " ・ " : "", (unsigned)m.laps.n);
  drawHeader(SC_LAPS, aux);
}
static void lapsBody() {
  const Model &m = *s_m;
  if (m.laps.n == 0) {
    mixDrawC(T22, 300, 250, "ラップがありません", C_LABEL);
    return;
  }
  const int best = bestLap(m);
  for (int i = 0; i < m.laps.n; i++) {
    const int y = kBodyY + lapTop(i) - g.scroll;
    const int h = kRowH + (i == g.lapOpen ? LAP_SEC_H : 0);
    if (y + h <= kBodyY || y >= gfx::H) continue;
    const LapRow &r = m.laps.lap[i];
    const bool isBest = i == best;
    if (isBest) gfx::fillRect(0, y, gfx::W, kRowH, C_TINT_YELLOW);
    const float cy = (float)y + kRowH * 0.5f;
    char t[16];
    snprintf(t, sizeof t, "L%u", (unsigned)r.no);
    gfx::drawText(FONT_CP28, 20, baseC(FONT_CP28, cy), t, isBest ? C_YELLOW : C_LABEL);
    fmtLap(t, sizeof t, r.ms);
    gfx::drawText(FONT_NUM46, 100, baseC(FONT_NUM46, cy), t, isBest ? C_YELLOW : (r.valid ? C_WHITE : C_LABEL));
    if (isBest) {
      const float bw = gfx::textWidth(FONT_CP22, "BEST") + 24;
      badge(580 - bw, cy - 17, bw, 34, 8, T22, "BEST", C_YELLOW, C_BG);
    } else if (best >= 0) {
      const float d = ((float)r.ms - (float)m.laps.lap[best].ms) / 1000.0f;
      fmtDelta(t, sizeof t, d, false);
      textR(FONT_NUM28, 580, baseC(FONT_NUM28, cy), t, C_SUB);
    }
    if (i == g.lapOpen) {  // sectors under the row
      const float sy = (float)(y + kRowH), scy = sy + LAP_SEC_H * 0.5f - 4;
      gfx::fillRect(0, (int)sy, gfx::W, LAP_SEC_H, C_PANEL);
      if (r.nSec == 0) {
        mixDraw(T18, 100, baseC(FONT_JP18, scy), "セクターなし", C_LABEL);
      } else {
        float x = 100;
        for (int k = 0; k < r.nSec && k < 4; k++) {
          char s[16];
          snprintf(s, sizeof s, "S%d", k + 1);
          x = gfx::drawText(FONT_CP18, x, baseC(FONT_CP18, scy), s, C_LABEL) + 8;
          fmtSec2(t, sizeof t, r.secMs[k]);
          x = gfx::drawText(FONT_NUM28, x, baseC(FONT_NUM28, scy), t, C_WHITE) + 34;
        }
      }
    }
    gfx::fillRect(0, y + h - 1, gfx::W, 1, C_LINE);
  }
}
static int lapsHit(const Model &m, int y) {
  const int cy = y - kBodyY + g.scroll;
  for (int i = 0; i < m.laps.n; i++) {
    const int t = lapTop(i), h = kRowH + (i == g.lapOpen ? LAP_SEC_H : 0);
    if (cy >= t && cy < t + h) return i;
  }
  return -1;
}

// ================================================================ maintenance
static uint16_t maintColour(uint8_t st) {
  return st == MS_OVERDUE ? C_RED : (st == MS_SOON ? C_YELLOW : C_GREEN);
}
static void maintHeader() {
  const Model &m = *s_m;
  char o[24], t[16], aux[96];
  if (m.maint.odoOk) fmtGrouped(o, sizeof o, m.maint.odoKm);
  else snprintf(o, sizeof o, "--");
  fmtTenths(t, sizeof t, m.maint.trackH);
  snprintf(aux, sizeof aux, "ODO %s km ・ サーキット %s h", o, t);
  drawHeader(SC_MAINT, aux);
}
// dialog: panel 520 x 290 with two 72 px buttons
static const float DLG_X = 40, DLG_Y = 112, DLG_W = 520, DLG_H = 300;
static const float DLG_BY = DLG_Y + DLG_H - 20 - 72, DLG_B1X = DLG_X + 20, DLG_BW = 230, DLG_B2X = DLG_X + DLG_W - 20 - 230;
static void maintBody() {
  const Model &m = *s_m;
  if (m.maint.n == 0) {
    mixDrawC(T22, 300, 250, "整備の項目がありません", C_LABEL);
    return;
  }
  for (int i = 0; i < m.maint.n; i++) {
    const int y = kBodyY + i * kRowH - g.scroll;
    if (y + kRowH <= kBodyY || y >= gfx::H) continue;
    const MaintItem &it = m.maint.item[i];
    const uint16_t c = maintColour(it.state);
    if (it.state == MS_OVERDUE) gfx::fillRect(0, y, gfx::W, kRowH, C_TINT_RED);
    const float base = (float)y + 38;
    const float rw = mixWidth(T26, it.remain);
    mixDraw(T26, 580 - rw, base, it.remain, c);
    const TextSet sets[2] = { T28, T22 };
    mixDrawFit(sets, 2, 20, base, 580 - rw - 16 - 20, it.name, C_WHITE);
    gfx::fillRoundRect(20, (float)y + 52, 560, 10, 5, C_OFF);
    const float f = it.state == MS_OVERDUE ? 1.0f : clampf(it.fraction, 0, 1);
    if (f > 0) gfx::fillRoundRect(20, (float)y + 52, 560 * f < 10 ? 10 : 560 * f, 10, 5, c);
    gfx::fillRect(0, y + kRowH - 1, gfx::W, 1, C_LINE);
  }
  if (g.maintDialog >= 0 && g.maintDialog < m.maint.n) {
    const MaintItem &it = m.maint.item[g.maintDialog];
    gfx::setAlpha(170);
    gfx::fillRect(0, kBodyY, gfx::W, kBodyH, C_BG);  // dim the rows behind
    gfx::setAlpha(255);
    gfx::frameRoundRect(DLG_X, DLG_Y, DLG_W, DLG_H, 12, 2, C_ORANGE, C_PANEL);
    const TextSet sets[2] = { T28, T22 };
    const float nw = mixWidth(T28, it.name);
    mixDrawFit(sets, 2, nw < DLG_W - 40 ? 300 - nw * 0.5f : DLG_X + 20, DLG_Y + 52, DLG_W - 40, it.name, C_WHITE);
    mixDrawC(T22, 300, DLG_Y + 98, it.remain, maintColour(it.state));
    mixDrawC(T22, 300, DLG_Y + 146, "整備を実施しましたか？", C_SUB);
    button(DLG_B1X, DLG_BY, DLG_BW, 72, T26, "実施済みにする", C_ORANGE, C_ORANGE, C_BG);
    button(DLG_B2X, DLG_BY, DLG_BW, 72, T26, "閉じる", C_LABEL, C_PANEL, C_WHITE);
  }
}
static bool inBox(int x, int y, float bx, float by, float bw, float bh) {
  return (float)x >= bx && (float)x < bx + bw && (float)y >= by && (float)y < by + bh;
}

// ================================================================ data
static const int DATA_TOP_H = 112, DATA_STAT_H = 52, CARD_H = 64, CARD_GAP = 8;
static int dataHeight(const Model &m) {
  return DATA_TOP_H + DATA_STAT_H + (m.data.nSessions ? m.data.nSessions * (CARD_H + CARD_GAP) : 40);
}
static void dataHeader() {
  const Model &m = *s_m;
  char aux[64];
  if (m.data.wifiOk) snprintf(aux, sizeof aux, "接続中: %s", m.data.ssid);
  else snprintf(aux, sizeof aux, "Wi-Fi 未接続");
  drawHeader(SC_DATA, aux);
}
static void dataBody() {
  const Model &m = *s_m;
  const DataPage &d = m.data;
  const float y0 = (float)(kBodyY - g.scroll);
  char t[128];
  if (d.wifiOk) {
    mixDraw(T18, 16, y0 + 28, "iPhone の Safari で開くアドレス", C_LABEL);
    snprintf(t, sizeof t, "%s.local", d.hostname);
    gfx::drawText(FONT_CP30, 16, y0 + 68, t, C_WHITE);
    textR(FONT_CP30, 584, y0 + 68, d.ip[0] ? d.ip : "---", C_WHITE);
    gfx::fillCircle(22, y0 + 92, 6, C_GREEN);
    snprintf(t, sizeof t, "接続中のネットワーク: %s", d.ssid);
    const TextSet sets[1] = { T18 };
    mixDrawFit(sets, 1, 36, y0 + 92 + (float)FONT_JP18.capH * 0.5f, 548, t, C_SUB);
  } else {
    mixDraw(T26, 16, y0 + 34, "Wi-Fi 未接続", C_RED);
    mixDraw(T18, 16, y0 + 62, "接続先（この順に探します）", C_LABEL);
    size_t u = 0;
    t[0] = 0;
    for (int i = 0; i < d.nNetworks && i < 4; i++) u += (size_t)snprintf(t + u, u < sizeof t ? sizeof t - u : 0, "%s%d. %s", i ? "   " : "", i + 1, d.network[i]);
    if (!d.nNetworks) snprintf(t, sizeof t, "wifi.json がありません");
    const TextSet sets[2] = { T22, T18 };
    mixDrawFit(sets, 2, 16, y0 + 96, 568, t, C_WHITE);
  }
  // remaining recording time and undownloaded sessions
  const float sy = y0 + DATA_TOP_H;
  if (d.remainHours >= 0) snprintf(t, sizeof t, "残り 約 %d 時間", (int)d.remainHours);
  else snprintf(t, sizeof t, "残り 約 -- 時間");
  float w1 = mixWidth(T22, t) + 32;
  badge(16, sy, w1, 38, 10, T22, t, C_PANEL, C_WHITE);
  snprintf(t, sizeof t, "未ダウンロード %u", (unsigned)d.undownloaded);
  float w2 = mixWidth(T22, t) + 32;
  badge(16 + w1 + 12, sy, w2, 38, 10, T22, t, C_PANEL, d.undownloaded ? C_BLUE : C_WHITE);
  // session cards
  const float cy0 = sy + DATA_STAT_H;
  if (!d.nSessions) {
    mixDraw(T18, 16, cy0 + 26, "セッションがありません", C_LABEL);
    return;
  }
  for (int i = 0; i < d.nSessions && i < 16; i++) {
    const SessionCard &s = d.session[i];
    const float y = cy0 + (float)(i * (CARD_H + CARD_GAP));
    if (y + CARD_H <= kBodyY || y >= gfx::H) continue;
    gfx::frameRoundRect(16, y, 568, CARD_H, 12, 2, s.downloaded ? C_OFF : C_BLUE, s.downloaded ? C_PANEL : C_TINT_BLUE);
    char dt[24];
    fmtDate(dt, sizeof dt, s.month, s.day, s.hour, s.minute);
    snprintf(t, sizeof t, "%s ・ %s", dt, s.course[0] ? s.course : "一般道");
    const char *bt = s.downloaded ? "ダウンロード済み" : "未ダウンロード";
    const float bw = mixWidth(T18, bt) + 28;
    badge(584 - 14 - bw, y + CARD_H * 0.5f - 15, bw, 30, 15, T18, bt, s.downloaded ? C_GREEN : C_DOT, s.downloaded ? C_BG : C_WHITE);
    const TextSet sets[2] = { T22, T18 };
    mixDrawFit(sets, 2, 32, y + 28, 584 - 14 - bw - 16 - 32, t, C_WHITE);
    char lt[48], bs[16];
    if (s.bestOk) {
      fmtLap(bs, sizeof bs, s.bestMs);
      snprintf(lt, sizeof lt, "%u 周 ・ ベスト %s", (unsigned)s.laps, bs);
    } else {
      snprintf(lt, sizeof lt, "%u 周", (unsigned)s.laps);
    }
    mixDraw(T18, 32, y + 52, lt, C_LABEL);
  }
}

// ================================================================ settings
enum SettingsRow { SR_COURSE = 0, SR_GATE, SR_SHIFT, SR_BRIGHT, SR_OIL, SR_RC, SR_REC, SR_WIFI, SR_INFO, SR_COUNT };
static int editorH(const Model &m) {
  switch (g.editor) {
    case SR_SHIFT: return 156;
    case SR_BRIGHT: return 92;
    case SR_COURSE: {
      const int n = 1 + (m.settings.nCourses > 8 ? 8 : m.settings.nCourses);
      return ((n + 1) / 2) * 64 + 20;
    }
    default: return 0;
  }
}
static int setTop(const Model &m, int i) {
  return i * kRowH + (g.editor >= 0 && i > g.editor ? editorH(m) : 0);
}
static int settingsHeight(const Model &m) {
  return SR_COUNT * kRowH + (g.editor >= 0 ? editorH(m) : 0);
}
static const char *courseLabel(const Model &m, char *b, size_t n) {
  const SettingsPage &s = m.settings;
  if (s.courseAuto) snprintf(b, n, "自動 ・ %s", m.courseName[0] ? m.courseName : "なし");
  else if (s.courseIndex >= 0 && s.courseIndex < s.nCourses) snprintf(b, n, "%s", s.courses[(int)s.courseIndex]);
  else snprintf(b, n, "なし");
  return b;
}
static void settingsHeader() {
  const Model &m = *s_m;
  char aux[48];
  snprintf(aux, sizeof aux, "v%s", m.fwVersion);
  drawHeader(SC_SETTINGS, aux);
}
static const uint16_t SHIFT_C[4] = { C_GREEN, C_YELLOW, C_RED, C_WHITE };
static uint16_t shiftVal(const ShiftPoints &sp, int i) {
  return i == 0 ? sp.green : (i == 1 ? sp.yellow : (i == 2 ? sp.red : sp.flash));
}
static void valueBadge(float xr, float cy, const char *s) {  // grey value badge, at most 330 px wide
  const float tw = mixWidth(T22, s), w = tw + 32 > 330 ? 330 : tw + 32;
  gfx::fillRoundRect(xr - w, cy - 20, w, 40, 10, C_OFF);
  const TextSet sets[2] = { T22, T18 };
  mixDrawFit(sets, 2, tw + 32 > 330 ? xr - w + 16 : xr - w * 0.5f - tw * 0.5f, baseC(FONT_JP22, cy), w - 32, s, C_WHITE);
}
// editor geometry (relative to its top)
static const float SH_CHIP_Y = 12, SH_CHIP_H = 56, SH_CHIP_W = 128, SH_BTN_Y = 80, SH_BTN_H = 64;
static float chipX(int i) {
  return 20 + (float)i * (SH_CHIP_W + 16);
}
static const float BR_Y = 14, BR_H = 64;
static void drawEditor(const Model &m, float top) {
  gfx::fillRect(0, (int)top, gfx::W, editorH(m), C_PANEL);
  char t[24];
  if (g.editor == SR_SHIFT) {
    for (int i = 0; i < 4; i++) {
      const float x = chipX(i), y = top + SH_CHIP_Y;
      if (i == g.shiftSel) gfx::fillRoundRect(x - 4, y - 4, SH_CHIP_W + 8, SH_CHIP_H + 8, 14, C_WHITE);
      if (i == g.shiftSel) gfx::fillRoundRect(x - 2, y - 2, SH_CHIP_W + 4, SH_CHIP_H + 4, 12, C_BG);
      snprintf(t, sizeof t, "%u", (unsigned)shiftVal(m.shift, i));
      badge(x, y, SH_CHIP_W, SH_CHIP_H, 10, T28, t, SHIFT_C[i], C_BG);
    }
    button(20, top + SH_BTN_Y, 176, SH_BTN_H, T28, "−100", C_LABEL, C_BG, C_WHITE);
    button(212, top + SH_BTN_Y, 176, SH_BTN_H, T28, "+100", C_LABEL, C_BG, C_WHITE);
    button(404, top + SH_BTN_Y, 176, SH_BTN_H, T26, "閉じる", C_DOT, C_BG, C_SUB);
  } else if (g.editor == SR_BRIGHT) {
    button(20, top + BR_Y, 130, BR_H, T28, "−10", C_LABEL, C_BG, C_WHITE);
    snprintf(t, sizeof t, "%u %%", (unsigned)m.settings.brightness);
    textC(FONT_NUM48, 228, baseC(FONT_NUM48, top + BR_Y + BR_H * 0.5f), t, C_WHITE);
    button(306, top + BR_Y, 130, BR_H, T28, "+10", C_LABEL, C_BG, C_WHITE);
    button(452, top + BR_Y, 128, BR_H, T26, "閉じる", C_DOT, C_BG, C_SUB);
  } else if (g.editor == SR_COURSE) {
    const SettingsPage &s = m.settings;
    const int n = 1 + (s.nCourses > 8 ? 8 : s.nCourses);
    for (int i = 0; i < n; i++) {
      const float x = 20 + (float)(i % 2) * 284, y = top + 12 + (float)(i / 2) * 64;
      const bool cur = i == 0 ? s.courseAuto : (!s.courseAuto && s.courseIndex == i - 1);
      const char *nm = i == 0 ? "自動" : s.courses[i - 1];
      gfx::frameRoundRect(x, y, 276, 56, 10, 3, cur ? C_YELLOW : C_LABEL, C_BG);
      const TextSet sets[2] = { T22, T18 };
      const float nw = mixWidth(T22, nm);
      mixDrawFit(sets, 2, nw < 252 ? x + 138 - nw * 0.5f : x + 12, baseC(FONT_JP22, y + 28), 252, nm, cur ? C_YELLOW : C_WHITE);
    }
  }
}
static void settingsBody() {
  const Model &m = *s_m;
  const SettingsPage &s = m.settings;
  static const char *const NAMES[SR_COUNT] = { "コース", "ゲート設定", "シフトポイント", "明るさ", "油温の向き", "RaceChrono", "記録", "Wi-Fi", "機器情報" };
  char b[96];
  for (int i = 0; i < SR_COUNT; i++) {
    const int y = kBodyY + setTop(m, i) - g.scroll;
    const int h = kRowH + (i == g.editor ? editorH(m) : 0);
    if (y + h <= kBodyY || y >= gfx::H) continue;
    const float cy = (float)y + kRowH * 0.5f;
    mixDraw(T28, 20, baseC(FONT_JP28, cy), NAMES[i], C_WHITE);
    const bool tappable = i <= SR_REC;
    if (tappable) gfx::drawText(FONT_CP28, 568, baseC(FONT_CP28, cy), "›", C_LABEL);
    const float xr = tappable ? 556 : 580;
    switch (i) {
      case SR_COURSE: valueBadge(xr, cy, courseLabel(m, b, sizeof b)); break;
      case SR_GATE: {
        size_t u = 0;
        static const char *const G[3] = { "S/F", "S1", "S2" };
        b[0] = 0;
        for (int k = 0; k < 3; k++)
          if (s.gateSet[k]) u += (size_t)snprintf(b + u, sizeof b - u, "%s%s", u ? " ・ " : "", G[k]);
        valueBadge(xr, cy, u ? b : "未設定");
        break;
      }
      case SR_SHIFT: {
        float x = xr;
        for (int k = 3; k >= 0; k--) {
          snprintf(b, sizeof b, "%u", (unsigned)shiftVal(m.shift, k));
          const float w = gfx::textWidth(FONT_CP22, b) + 18;
          x -= w;
          badge(x, cy - 20, w, 40, 10, T22, b, SHIFT_C[k], C_BG);
          x -= 6;
        }
        break;
      }
      case SR_BRIGHT:
        snprintf(b, sizeof b, "%u %%", (unsigned)s.brightness);
        valueBadge(xr, cy, b);
        break;
      case SR_OIL: valueBadge(xr, cy, s.oilInvert ? "反転" : "標準"); break;
      case SR_RC: valueBadge(xr, cy, s.raceChrono ? "使う" : "使わない"); break;
      case SR_REC: valueBadge(xr, cy, s.recording ? "記録中" : "停止中"); break;
      case SR_WIFI: valueBadge(xr, cy, m.data.wifiOk ? m.data.ssid : "未接続"); break;
      case SR_INFO:
        snprintf(b, sizeof b, "v%s", m.fwVersion);
        valueBadge(xr, cy, b);
        break;
      default: break;
    }
    if (i == g.editor) drawEditor(m, (float)(y + kRowH));
    gfx::fillRect(0, y + h - 1, gfx::W, 1, C_LINE);
  }
}
static void settingsTap(const Model &m, int x, int y, ActionQueue &out) {
  const int cy = y - kBodyY + g.scroll;
  if (g.editor >= 0) {  // inside the open editor
    const int et = setTop(m, g.editor) + kRowH, eh = editorH(m);
    if (cy >= et && cy < et + eh) {
      const float ly = (float)(cy - et);
      Action a;
      a.kind = ACT_SETTING;
      if (g.editor == SR_SHIFT) {
        for (int i = 0; i < 4; i++)
          if (inBox(x, (int)ly, chipX(i), SH_CHIP_Y, SH_CHIP_W, SH_CHIP_H)) g.shiftSel = (int8_t)i;
        if (ly >= SH_BTN_Y && ly < SH_BTN_Y + SH_BTN_H) {
          if (x >= 404) {
            g.editor = -1;
            return;
          }
          const int step = x < 204 ? -100 : (x >= 212 && x < 388 ? 100 : 0);
          if (!step) return;
          const ShiftPoints &sp = m.shift;
          const int i = g.shiftSel;
          int v = (int)shiftVal(sp, i) + step;
          // keep green < yellow < red <= flash inside 1000..9000
          const int lo = i == 0 ? 1000 : (int)shiftVal(sp, i - 1) + (i == 3 ? 0 : 100);
          const int hi = i == 3 ? 9000 : (int)shiftVal(sp, i + 1) - (i == 2 ? 0 : 100);
          if (v < lo || v > hi) return;
          a.arg = (uint8_t)(SET_SHIFT_GREEN + i);
          a.value = v;
          out.push(a);
        }
      } else if (g.editor == SR_BRIGHT) {
        if (ly < BR_Y || ly >= BR_Y + BR_H) return;
        if (x >= 452) {
          g.editor = -1;
          return;
        }
        int v = (int)m.settings.brightness;
        if (x < 150) v -= 10;
        else if (x >= 306 && x < 436) v += 10;
        else return;
        if (v < 10 || v > 100) return;
        a.arg = SET_BRIGHTNESS;
        a.value = v;
        out.push(a);
      } else if (g.editor == SR_COURSE) {
        const int n = 1 + (m.settings.nCourses > 8 ? 8 : m.settings.nCourses);
        for (int i = 0; i < n; i++) {
          const float bx = 20 + (float)(i % 2) * 284, by = 12 + (float)(i / 2) * 64;
          if (!inBox(x, (int)ly, bx, by, 276, 56)) continue;
          a.arg = SET_COURSE;
          a.value = i - 1;
          out.push(a);
          g.editor = -1;
          return;
        }
      }
      return;
    }
  }
  for (int i = 0; i < SR_COUNT; i++) {
    const int t = setTop(m, i);
    if (cy < t || cy >= t + kRowH) continue;
    Action a;
    switch (i) {
      case SR_COURSE:
      case SR_SHIFT:
      case SR_BRIGHT: {
        g.editor = (int8_t)(g.editor == i ? -1 : i);
        if (g.editor >= 0) {  // scroll so that the editor is fully visible
          const int bottom = setTop(m, i) + kRowH + editorH(m);
          if (bottom - g.scroll > kBodyH) g.scroll = (int16_t)(bottom - kBodyH);
        }
        break;
      }
      case SR_GATE:
        a.kind = ACT_PAGE;
        a.arg = SC_GATE;
        out.push(a);
        break;
      case SR_OIL:
        a.kind = ACT_SETTING;
        a.arg = SET_OIL_INVERT;
        a.value = m.settings.oilInvert ? 0 : 1;
        out.push(a);
        break;
      case SR_RC:
        a.kind = ACT_SETTING;
        a.arg = SET_RACECHRONO;
        a.value = m.settings.raceChrono ? 0 : 1;
        out.push(a);
        break;
      case SR_REC:
        a.kind = m.settings.recording ? ACT_RECORD_STOP : ACT_RECORD_RESUME;
        out.push(a);
        break;
      default: break;
    }
    return;
  }
}

// ================================================================ gate setting
static const float TRK_X0 = 0, TRK_Y0 = 48, TRK_W = 424, TRK_H = 402, TRK_PAD = 18;
static const float GB_X = 428, GB_W = 156, GB_H = 60;
static const float GB_Y[4] = { 170, 238, 306, 374 };
struct Proj {
  bool ok;
  int32_t lat0, lon0;
  float kx, ky;  // metres per 1e-7 deg
  float s;       // pixels per metre
  float ox, oy;  // screen position of (lat0, lon0)
};
static Proj s_proj;
static void project(const Model &m) {
  Proj &p = s_proj;
  p.ok = false;
  const GatePage &gp = m.gate;
  if (gp.n < 2) return;
  int32_t la0 = gp.pt[0].latE7, la1 = la0, lo0 = gp.pt[0].lonE7, lo1 = lo0;
  for (int i = 1; i < gp.n; i++) {
    const GatePoint &q = gp.pt[i];
    if (q.latE7 < la0) la0 = q.latE7;
    if (q.latE7 > la1) la1 = q.latE7;
    if (q.lonE7 < lo0) lo0 = q.lonE7;
    if (q.lonE7 > lo1) lo1 = q.lonE7;
  }
  p.lat0 = la0 + (la1 - la0) / 2;
  p.lon0 = lo0 + (lo1 - lo0) / 2;
  p.ky = 0.0111319f;  // m per 1e-7 deg latitude
  p.kx = p.ky * cosf((float)p.lat0 * 1e-7f * 3.14159265f / 180.0f);
  const float wm = (float)(lo1 - lo0) * p.kx, hm = (float)(la1 - la0) * p.ky;
  const float sx = (TRK_W - 2 * TRK_PAD) / (wm > 1 ? wm : 1), sy = (TRK_H - 2 * TRK_PAD - 24) / (hm > 1 ? hm : 1);
  p.s = sx < sy ? sx : sy;
  p.ox = TRK_X0 + TRK_W * 0.5f;
  p.oy = TRK_Y0 + (TRK_H - 24) * 0.5f;
  p.ok = true;
}
static gfx::Pt toScreen(int32_t lat, int32_t lon) {
  const Proj &p = s_proj;
  return gfx::Pt{ p.ox + (float)(lon - p.lon0) * p.kx * p.s, p.oy - (float)(lat - p.lat0) * p.ky * p.s };
}
static float headingAt(const Model &m, int i) {  // direction of travel at point i [deg, 0 = north]
  const GatePage &gp = m.gate;
  const int a = i > 0 ? i - 1 : i, b = i + 1 < gp.n ? i + 1 : i;
  const float dx = (float)(gp.pt[b].lonE7 - gp.pt[a].lonE7) * s_proj.kx, dy = (float)(gp.pt[b].latE7 - gp.pt[a].latE7) * s_proj.ky;
  float h = atan2f(dx, dy) * 180.0f / 3.14159265f;
  return h < 0 ? h + 360.0f : h;
}
// gate line across the track: centre c, heading h, width w metres
static void gateLine(gfx::Pt c, float headingDeg, float widthM, float thick, uint16_t col) {
  const float a = headingDeg * 3.14159265f / 180.0f;
  const float ux = cosf(a), uy = sinf(a);  // across the travel direction (screen: x east, y south)
  float half = widthM * 0.5f * s_proj.s;
  if (half < 16) half = 16;  // a 30 m gate is only a few pixels on a long track
  gfx::fillQuad(c.x - ux * half, c.y - uy * half, c.x + ux * half, c.y + uy * half, thick, col);
}
static void gateHeader() {
  const Model &m = *s_m;
  float x = mixDraw(T22, 16, baseC(FONT_JP22, 24), "‹ 設定", C_YELLOW);
  x = mixDraw(T22, x + 22, baseC(FONT_JP22, 24), "ゲート設定", C_WHITE);
  char aux[128], d[24];
  d[0] = 0;
  if (m.gate.startOk) fmtDate(d, sizeof d, m.gate.month, m.gate.day, m.gate.hour, m.gate.minute);
  snprintf(aux, sizeof aux, "%s%s%s の軌跡", m.gate.course, m.gate.course[0] && d[0] ? " " : "", d);
  const TextSet sets[1] = { T18 };
  mixDrawFit(sets, 1, x + 16, baseC(FONT_JP18, 24), 584 - x - 16, aux, C_LABEL);
  gfx::fillRect(0, 48, gfx::W, 1, C_OFF);
}
static void gateTrack() {
  const Model &m = *s_m;
  const GatePage &gp = m.gate;
  if (!s_proj.ok) {
    mixDrawC(T22, TRK_X0 + TRK_W * 0.5f, 250, "前回の軌跡がありません", C_LABEL);
    return;
  }
  // the track as one polygon set: a quad per segment and a disc per joint (overlaps count once)
  const float half = 3.5f;
  gfx::polyBegin(rectXY((int)TRK_X0, (int)TRK_Y0 + 1, (int)(TRK_X0 + TRK_W), (int)(TRK_Y0 + TRK_H)));
  gfx::Pt prev = toScreen(gp.pt[0].latE7, gp.pt[0].lonE7);
  for (int i = 1; i <= gp.n; i++) {
    const int j = i % gp.n;  // closed loop back to the start
    gfx::Pt q = toScreen(gp.pt[j].latE7, gp.pt[j].lonE7);
    const float dx = q.x - prev.x, dy = q.y - prev.y, l = sqrtf(dx * dx + dy * dy);
    if (l > 0.3f) {
      const float nx = -dy / l * half, ny = dx / l * half;
      gfx::Pt quad[4] = { { prev.x + nx, prev.y + ny }, { q.x + nx, q.y + ny }, { q.x - nx, q.y - ny }, { prev.x - nx, prev.y - ny } };
      gfx::polyAdd(quad, 4);
      gfx::Pt oct[8];
      for (int k = 0; k < 8; k++) oct[k] = { q.x + half * cosf((float)k * 0.7853982f), q.y + half * sinf((float)k * 0.7853982f) };
      gfx::polyAdd(oct, 8);
      prev = q;
    }
  }
  gfx::polyFill(C_DOT);
  // direction arrow at the start
  {
    gfx::Pt a = toScreen(gp.pt[0].latE7, gp.pt[0].lonE7);
    const float h = headingAt(m, 0) * 3.14159265f / 180.0f;
    const float fx = sinf(h), fy = -cosf(h);
    gfx::Pt tri[3] = { { a.x + fx * 14 - fy * 0, a.y + fy * 14 }, { a.x - fy * 8, a.y + fx * 8 }, { a.x + fy * 8, a.y - fx * 8 } };
    gfx::fillPoly(tri, 3, C_LABEL);
  }
  static const char *const GN[3] = { "S/F", "S1", "S2" };
  for (int k = 0; k < 3; k++) {
    const GateMark &gm = gp.gate[k];
    if (!gm.set) continue;
    gfx::Pt c = toScreen(gm.latE7, gm.lonE7);
    gateLine(c, gm.headingDeg, gm.widthM, k == 0 ? 5.0f : 3.0f, k == 0 ? C_YELLOW : C_TICK);
    gfx::drawText(FONT_CP18, c.x + 12, c.y - 12, GN[k], k == 0 ? C_YELLOW : C_LABEL);
  }
  if (g.gatePick >= 0 && g.gatePick < gp.n) {
    gfx::Pt c = toScreen(gp.pt[g.gatePick].latE7, gp.pt[g.gatePick].lonE7);
    gateLine(c, headingAt(m, g.gatePick), 30.0f, 5.0f, C_YELLOW);
    gfx::fillCircle(c.x, c.y, 8.0f, C_WHITE);
    gfx::fillCircle(c.x, c.y, 5.5f, C_YELLOW);
  }
  mixDraw(T18, 12, 440, "軌跡をタップ → 進行方向に直角のゲートを置く", C_LABEL);
}
static void gatePanel() {
  const bool pick = g.gatePick >= 0;
  mixDraw(T22, GB_X, 84, "ゲートを置く", C_YELLOW);
  mixDraw(T18, GB_X, 112, pick ? "位置: 軌跡上の点" : "位置: 軌跡をタップ", C_LABEL);
  mixDraw(T18, GB_X, 136, "幅 S/F 50・S 30 m", C_LABEL);
  mixDraw(T18, GB_X, 160, "向き: 軌跡どおり", C_LABEL);
  static const char *const L[3] = { "S/F に設定", "S1 に設定", "S2 に設定" };
  for (int k = 0; k < 3; k++) {
    uint16_t bc = k == 0 ? C_YELLOW : C_LABEL, fc = k == 0 ? C_YELLOW : C_WHITE;
    if (!pick) {
      bc = C_OFF;
      fc = C_DOT;
    }
    button(GB_X, GB_Y[k], GB_W, GB_H, T22, L[k], bc, C_BG, fc);
  }
  button(GB_X, GB_Y[3], GB_W, GB_H, T22, "取消", pick ? C_DOT : C_OFF, C_BG, pick ? C_LABEL : C_DOT);
}
static void gateTap(const Model &m, int x, int y, ActionQueue &out) {
  if (y < 48) {
    if (x < 130) {
      Action a;
      a.kind = ACT_PAGE;
      a.arg = SC_SETTINGS;
      out.push(a);
    }
    return;
  }
  if ((float)x < TRK_X0 + TRK_W) {  // pick the nearest track point within 28 px
    if (!s_proj.ok) return;
    int best = -1;
    float bd = 28.0f * 28.0f;
    for (int i = 0; i < m.gate.n; i++) {
      gfx::Pt p = toScreen(m.gate.pt[i].latE7, m.gate.pt[i].lonE7);
      const float d = (p.x - (float)x) * (p.x - (float)x) + (p.y - (float)y) * (p.y - (float)y);
      if (d < bd) {
        bd = d;
        best = i;
      }
    }
    if (best >= 0) g.gatePick = (int16_t)best;
    return;
  }
  for (int k = 0; k < 4; k++) {
    if (!inBox(x, y, GB_X, GB_Y[k], GB_W, GB_H)) continue;
    if (g.gatePick < 0 || g.gatePick >= m.gate.n) return;
    if (k < 3) {
      Action a;
      a.kind = ACT_GATE_SET;
      a.arg = (uint8_t)k;
      a.latE7 = m.gate.pt[g.gatePick].latE7;
      a.lonE7 = m.gate.pt[g.gatePick].lonE7;
      a.headingDeg = headingAt(m, g.gatePick);
      a.value = k == 0 ? 50 : 30;  // RaceChrono's widths: S/F 50 m (pit lane counts, 実装計画 §0), splits 30 m
      out.push(a);
    }
    g.gatePick = -1;
    return;
  }
}

// ================================================================ dispatch
static void bodyKeyLaps(uint32_t &h, const Model &m) {
  h = hashBytes(h, m.laps.lap, sizeof(LapRow) * m.laps.n);
  h = hashU(hashU(h, m.laps.n), (uint32_t)g.lapOpen);
}
static void pageDraw(uint8_t sc, bool head) {
  switch (sc) {
    case SC_LAPS:
      if (head) lapsHeader();
      else lapsBody();
      break;
    case SC_MAINT:
      if (head) maintHeader();
      else maintBody();
      break;
    case SC_DATA:
      if (head) dataHeader();
      else dataBody();
      break;
    case SC_SETTINGS:
      if (head) settingsHeader();
      else settingsBody();
      break;
    default: break;
  }
}
static uint8_t s_sc = 0;
static void headFn() {
  pageDraw(s_sc, true);
}
static void bodyFn() {
  pageDraw(s_sc, false);
}

void pagesFrame(const Model &m, bool full) {
  s_m = &m;
  s_sc = m.screen;
  if (m.screen == SC_GATE) {
    uint32_t kh = hashStr(kHash0, m.gate.course);
    kh = hashU(hashU(kh, m.gate.startOk), (uint32_t)m.gate.month << 24 | (uint32_t)m.gate.day << 16 | (uint32_t)m.gate.hour << 8 | m.gate.minute);
    uint32_t kb = hashBytes(kHash0, m.gate.pt, sizeof(GatePoint) * m.gate.n);
    kb = hashBytes(hashU(hashU(kb, m.gate.n), (uint32_t)g.gatePick), m.gate.gate, sizeof m.gate.gate);
    if (full || kb != s_keyBody) project(m);
    if (full || kh != s_keyHead) redraw(rectXY(0, 0, gfx::W, 49), gateHeader);
    if (full || kb != s_keyBody) {
      redraw(rectXY(0, 49, (int)(TRK_X0 + TRK_W), gfx::H), gateTrack);
      redraw(rectXY((int)(TRK_X0 + TRK_W), 49, gfx::W, gfx::H), gatePanel);
    }
    s_keyHead = kh;
    s_keyBody = kb;
    return;
  }
  if (g.scroll < 0) g.scroll = (int16_t)(m.screen == SC_LAPS ? pageScrollMax(m) : 0);
  uint32_t kh = kHash0, kb = hashU(kHash0, (uint32_t)g.scroll);
  switch (m.screen) {
    case SC_LAPS:
      kh = hashStr(hashU(kh, m.laps.n), m.laps.course);
      kh = hashU(hashU(kh, m.laps.startOk), (uint32_t)m.laps.month << 24 | (uint32_t)m.laps.day << 16 | (uint32_t)m.laps.hour << 8 | m.laps.minute);
      bodyKeyLaps(kb, m);
      break;
    case SC_MAINT:
      kh = hashU(hashU(hashU(kh, m.maint.odoOk), m.maint.odoKm), (uint32_t)lroundf(m.maint.trackH * 10));
      kb = hashBytes(hashU(hashU(kb, m.maint.n), (uint32_t)g.maintDialog), m.maint.item, sizeof(MaintItem) * m.maint.n);
      break;
    case SC_DATA:
      kh = hashStr(hashU(kh, m.data.wifiOk), m.data.ssid);
      kb = hashBytes(kb, &m.data, sizeof m.data);
      break;
    case SC_SETTINGS:
      kh = hashStr(kh, m.fwVersion);
      kb = hashBytes(hashU(hashU(kb, (uint32_t)g.editor), (uint32_t)g.shiftSel), &m.settings, sizeof m.settings);
      kb = hashBytes(kb, &m.shift, sizeof m.shift);
      kb = hashStr(hashStr(hashStr(kb, m.courseName), m.fwVersion), m.data.ssid);
      kb = hashU(kb, m.data.wifiOk);
      break;
    default: break;
  }
  if (full || kh != s_keyHead) redraw(rectXY(0, 0, gfx::W, kBodyY), headFn);
  if (full || kb != s_keyBody) redraw(rectXY(0, kBodyY, gfx::W, gfx::H), bodyFn);
  s_keyHead = kh;
  s_keyBody = kb;
}

int pageScrollMax(const Model &m) {
  int h = 0;
  switch (m.screen) {
    case SC_LAPS: h = lapsHeight(m); break;
    case SC_MAINT: h = m.maint.n * kRowH; break;
    case SC_DATA: h = dataHeight(m); break;
    case SC_SETTINGS: h = settingsHeight(m); break;
    default: return 0;
  }
  return h > kBodyH ? h - kBodyH : 0;
}

bool pageModal() {
  return g.shown == SC_MAINT && g.maintDialog >= 0;
}

void pageTap(const Model &m, int x, int y, ActionQueue &out) {
  switch (m.screen) {
    case SC_LAPS: {
      if (y < kBodyY) return;
      const int i = lapsHit(m, y);
      if (i >= 0) g.lapOpen = (int8_t)(g.lapOpen == i ? -1 : i);
      break;
    }
    case SC_MAINT: {
      if (g.maintDialog >= 0) {
        if (g.maintDialog < m.maint.n && inBox(x, y, DLG_B1X, DLG_BY, DLG_BW, 72)) {
          Action a;
          a.kind = ACT_MAINT_DONE;
          a.arg = (uint8_t)g.maintDialog;
          snprintf(a.id, sizeof a.id, "%s", m.maint.item[g.maintDialog].id);
          out.push(a);
          g.maintDialog = -1;
        } else if (inBox(x, y, DLG_B2X, DLG_BY, DLG_BW, 72)) {
          g.maintDialog = -1;
        }
        return;
      }
      if (y < kBodyY) return;
      const int i = (y - kBodyY + g.scroll) / kRowH;
      if (i >= 0 && i < m.maint.n) g.maintDialog = (int8_t)i;
      break;
    }
    case SC_SETTINGS:
      if (y >= kBodyY) settingsTap(m, x, y, out);
      break;
    case SC_GATE: gateTap(m, x, y, out); break;
    default: break;
  }
}

}  // namespace in
}  // namespace ui
