// ui/scr_pages.cpp - boot self-test (§3) and the stopped pages: laps, maintenance, data, settings, gate, Wi-Fi (§7),
// 走行記録 (odometer log, round gauge 2.15.x)
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
    if (!d.nNetworks) snprintf(t, sizeof t, "未登録（設定 → Wi-Fi）");
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
enum SettingsRow { SR_ODO = 0, SR_COURSE, SR_GATE, SR_SHIFT, SR_BRIGHT, SR_OIL, SR_RC, SR_REC, SR_WIFI, SR_INFO, SR_COUNT };
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
  static const char *const NAMES[SR_COUNT] = { "走行記録", "コース", "ゲート設定", "シフトポイント", "明るさ", "油温の向き", "RaceChrono", "記録", "Wi-Fi", "機器情報" };
  char b[96];
  for (int i = 0; i < SR_COUNT; i++) {
    const int y = kBodyY + setTop(m, i) - g.scroll;
    const int h = kRowH + (i == g.editor ? editorH(m) : 0);
    if (y + h <= kBodyY || y >= gfx::H) continue;
    const float cy = (float)y + kRowH * 0.5f;
    mixDraw(T28, 20, baseC(FONT_JP28, cy), NAMES[i], C_WHITE);
    const bool tappable = i <= SR_REC || i == SR_WIFI;  // SR_ODO is first
    if (tappable) gfx::drawText(FONT_CP28, 568, baseC(FONT_CP28, cy), "›", C_LABEL);
    const float xr = tappable ? 556 : 580;
    switch (i) {
      case SR_ODO: {  // the odometer now (round gauge 2.15.0: 「走行記録」(今の ODO))
        char o[16];
        fmtGrouped(o, sizeof o, m.odo.cur);
        if (m.odo.curOk) snprintf(b, sizeof b, "ODO %s km", o);
        else snprintf(b, sizeof b, "ODO 受信待ち");
        valueBadge(xr, cy, b);
        break;
      }
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
      case SR_ODO:
      case SR_GATE:
      case SR_WIFI:
        a.kind = ACT_PAGE;
        a.arg = i == SR_ODO ? SC_ODO : (i == SR_GATE ? SC_GATE : SC_WIFI);
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

// ================================================================ Wi-Fi (settings > Wi-Fi)
// Views: the registered networks (list), the scan result, the on-screen keyboard, the join result. The typed
// text stays in s_wt until ACT_WIFI_SAVE and is wiped as soon as it is not needed (キャンセル, save, a result
// without 入力しなおす, leaving the page). Drawn as items with boxes and keys (header or text field, body, hint,
// keys, buttons, dialog), redrawn bottom-up inside the clip like the street screen.
static const int WF_ROW0 = 92;     // list: first row, under the status line and the label
static const int WF_HINT_Y = 398;  // scan: the hint under the list
static const int WF_BACK_W = 150;  // tap width of ‹ 設定 / ‹ 戻る
static const float WF_RESCAN_X = 452, WF_RESCAN_Y = 9, WF_RESCAN_W = 132, WF_RESCAN_H = 46;
static const float WF_SHOW_X = 488, WF_SHOW_Y = 9, WF_SHOW_W = 96, WF_SHOW_H = 46;
static const float WF_TEXT_X = 30, WF_TEXT_R = 472;  // text field (in the band): title line, typed text
// keyboard: 5 rows on a 10-column grid; keys 53.2 x 54 (pitch 57.2 x 58); buttons 64 px under it
static const float KB_X0 = 16, KB_PX = 57.2f, KB_GAP = 4, KB_Y0 = 74, KB_PY = 58, KB_KH = 54;
static const float KB_BY = 370, KB_BH = 64, KB_OKX = 16, KB_OKW = 360, KB_CANX = 392, KB_CANW = 192;
static const float WR_BY = 362, WR_BH = 72;  // result buttons: 16..292 and 308..584, a single one 170..430
static const char *const WIFI_HINT[2] = { "2.4 GHz のネットワークだけ表示されます", "（iPhone のインターネット共有は「互換性を優先」をオン）" };

struct WifiText {
  char text[65];     // typed: a password (up to 64 characters) or a network name (up to 32)
  uint8_t len;
  uint16_t rev;      // changes with every edit: redraw key of the field (the text itself is never hashed)
  char target[33];   // network the password is for, then the network of the join result
  char retry[65];    // the saved password while its result can still offer 入力しなおす
  bool open;         // the saved network has no password
  char dlgSsid[33];  // network of the open dialog
};
static WifiText s_wt;
static void wipe(void *p, size_t n) {  // volatile: the optimiser keeps the stores
  volatile uint8_t *b = (volatile uint8_t *)p;
  while (n--) *b++ = 0;
}
void wifiReset() {
  g.wifiView = WV_LIST;
  g.wifiDialog = WD_NONE;
  g.wifiMode = KM_PASS;
  g.wifiShift = g.wifiSym = g.wifiHidden = false;
  wipe(&s_wt, sizeof s_wt);
}

// ---- keyboard layout. Layers: lower, upper (⇧ toggles and stays), symbols. Digits on every layer; - . _ @ on
// lower / upper, the other 28 punctuation characters on symbols: all of 0x20..0x7E can be typed.
//   1 2 3 4 5 6 7 8 9 0 | q w e r t y u i o p | a s d f g h j k l - | ⇧ z x c v b n m ⌫ | #+= space . _ @
//   symbols: 1..0 | ! @ # $ % ^ & * ( ) | [ ] { } < > ' " ` ~ | + = / \ | : ; ⌫ | abc space . , ?
enum : uint8_t { K_SHIFT = 1, K_LAYER, K_BACK };  // control keys (codes below 0x20)
struct KbKey {
  uint8_t code;          // printable ASCII or K_*
  uint8_t row, c2, w2;   // row 0..4, start and width in half columns
};
static const char *const KB_CHARS[3][5] = {
  { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm", "._@" },
  { "1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM", "._@" },
  { "1234567890", "!@#$%^&*()", "[]{}<>'\"`~", "+=/\\|:;", ".,?" },
};
static const int KB_MAX = 48;
static int kbKeys(int layer, KbKey *k) {
  int n = 0;
  for (int r = 0; r < 5; r++) {
    if (r == 3 && layer != 2) k[n++] = KbKey{ K_SHIFT, 3, 0, 3 };
    if (r == 4) {
      k[n++] = KbKey{ K_LAYER, 4, 0, 4 };
      k[n++] = KbKey{ ' ', 4, 4, 10 };
    }
    const int c0 = r == 3 ? 3 : (r == 4 ? 14 : 0);
    const char *s = KB_CHARS[layer][r];
    for (int i = 0; s[i]; i++) k[n++] = KbKey{ (uint8_t)s[i], (uint8_t)r, (uint8_t)(c0 + 2 * i), 2 };
    if (r == 3) k[n++] = KbKey{ K_BACK, 3, 17, 3 };
  }
  return n;
}
static void kbRect(const KbKey &k, float &x, float &y, float &w) {  // key height KB_KH
  x = KB_X0 + (float)k.c2 * KB_PX * 0.5f;
  y = KB_Y0 + (float)k.row * KB_PY;
  w = (float)k.w2 * KB_PX * 0.5f - KB_GAP;
}
static int kbLayer() {
  return g.wifiSym ? 2 : (g.wifiShift ? 1 : 0);
}
static bool kbValid() {  // password: 8..63 characters or 64 hex digits; name: 1..32 bytes
  if (g.wifiMode == KM_NAME) return s_wt.len >= 1 && s_wt.len <= 32;
  if (s_wt.len >= 8 && s_wt.len <= 63) return true;
  if (s_wt.len != 64) return false;
  for (int i = 0; i < 64; i++) {
    const char c = s_wt.text[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

// ---- small drawings
static void shiftIcon(float cx, float cy, uint16_t c) {  // ⇧
  gfx::Pt p[7] = { { cx, cy - 12 }, { cx + 12, cy + 1 }, { cx + 5, cy + 1 }, { cx + 5, cy + 11 }, { cx - 5, cy + 11 }, { cx - 5, cy + 1 }, { cx - 12, cy + 1 } };
  gfx::fillPoly(p, 7, c);
}
static void backIcon(float cx, float cy, uint16_t c, uint16_t bg) {  // ⌫
  gfx::Pt p[5] = { { cx - 17, cy }, { cx - 7, cy - 11 }, { cx + 16, cy - 11 }, { cx + 16, cy + 11 }, { cx - 7, cy + 11 } };
  gfx::fillPoly(p, 5, c);
  crossMark(cx + 4, cy, 17, bg);
}
static int rssiLevel(int8_t rssi) {  // 1..4 bars
  return rssi >= -55 ? 4 : (rssi >= -65 ? 3 : (rssi >= -75 ? 2 : 1));
}
static void signalBars(float x, float yb, int level) {  // 4 bars of growing height, bottom at yb
  for (int i = 0; i < 4; i++) {
    const float h = 7.0f + 5.0f * (float)i;
    gfx::fillRoundRect(x + 7.0f * (float)i, yb - h, 5, h, 1.5f, i < level ? C_WHITE : C_DOT);
  }
}
static void lockIcon(float cx, float cy, uint16_t c) {  // padlock, about 16 x 19
  gfx::fillArc(cx, cy - 2, 3.6f, 6.4f, 180, 360, c);
  gfx::fillRoundRect(cx - 8, cy - 2, 16, 12, 2.5f, c);
}
// centred at cx with the first set that fits in maxW, else cut with "…"
static void drawFitC(const TextSet *sets, int n, float cx, float base, float maxW, const char *s, uint16_t c) {
  for (int i = 0; i < n; i++) {
    const float w = mixWidth(sets[i], s);
    if (w <= maxW) {
      mixDraw(sets[i], cx - w * 0.5f, base, s, c);
      return;
    }
  }
  mixDrawFit(sets, n, cx - maxW * 0.5f, base, maxW, s, c);
}
static uint8_t wifiShown(const Model &m) {  // join state for the result view (IDLE / other network: still trying)
  const WifiPage &w = m.wifi;
  if (w.join == WJ_IDLE || w.join == WJ_TRYING || strcmp(w.joinSsid, s_wt.target) != 0) return WJ_TRYING;
  return w.join;
}
static bool wifiRetryable(uint8_t st) {
  return !s_wt.open && (st == WJ_BAD_PASSWORD || st == WJ_FAILED);
}
static bool wifiScanList(const WifiPage &w) {  // the scan result is shown (else one line: searching / none)
  return w.scanOk && !w.scanning && w.nFound > 0;
}
static int wifiScanRows(const WifiPage &w) {  // rows before 名前を入力
  return wifiScanList(w) ? (w.nFound > 16 ? 16 : w.nFound) : 1;
}
static int wifiSaved(const WifiPage &w) {
  return w.nSaved > 8 ? 8 : w.nSaved;
}

// ---- items
static void wifiHeader() {  // list, scan, result
  const Model &m = *s_m;
  const PageStyle &st = pageStyle(SC_WIFI);
  drawHeaderBand(st);
  const float cy = 32;
  float x = mixDraw(T22, 30, baseC(FONT_JP22, cy), g.wifiView == WV_SCAN ? "‹ 戻る" : "‹ 設定", C_YELLOW);
  x = gfx::drawText(FONT_CP30, x + 22, baseC(FONT_CP30, cy), "Wi-Fi", st.color);
  if (g.wifiView != WV_SCAN) return;
  const TextSet sets[1] = { T18 };
  mixDrawFit(sets, 1, x + 16, baseC(FONT_JP18, cy), WF_RESCAN_X - 16 - (x + 16), "ネットワークを追加", C_LABEL);
  const bool can = !m.wifi.scanning;
  button(WF_RESCAN_X, WF_RESCAN_Y, WF_RESCAN_W, WF_RESCAN_H, T22, "再検索", can ? C_LABEL : C_OFF, C_BG, can ? C_WHITE : C_DOT);
}
static void wifiField() {  // keyboard view: the band holds the title, the typed text and 表示 / 隠す
  drawHeaderBand(pageStyle(SC_WIFI));
  char t[80], cnt[16];
  snprintf(cnt, sizeof cnt, "%u 文字", (unsigned)s_wt.len);
  const float tb = baseC(FONT_JP18, 19), cw = mixWidth(T18, cnt);
  mixDraw(T18, WF_TEXT_R - cw, tb, cnt, C_LABEL);
  if (g.wifiMode == KM_NAME) snprintf(t, sizeof t, "ネットワーク名");
  else snprintf(t, sizeof t, "パスワード ・ %s", s_wt.target);
  const TextSet sets[1] = { T18 };
  mixDrawFit(sets, 1, WF_TEXT_X, tb, WF_TEXT_R - cw - 16 - WF_TEXT_X, t, C_SUB);
  if (g.wifiMode == KM_PASS) button(WF_SHOW_X, WF_SHOW_Y, WF_SHOW_W, WF_SHOW_H, T22, g.wifiHidden ? "表示" : "隠す", C_LABEL, C_BG, C_WHITE);
  // the text; when it is longer than the field its start is cut ("…"), so the end and the cursor stay visible
  const float cy = 46, base = baseC(FONT_CP26, cy), right = WF_TEXT_R - 8;
  float x = WF_TEXT_X;
  if (g.wifiHidden) {  // one dot per character; the text itself is not drawn
    const float pitch = 15;
    int first = 0;
    if (WF_TEXT_X + (float)s_wt.len * pitch > right) {
      x = gfx::drawText(FONT_CP26, x, base, "…", C_LABEL) + 4;
      first = s_wt.len - (int)((right - x) / pitch);
    }
    for (int i = first; i < s_wt.len; i++, x += pitch) gfx::fillCircle(x + 6, cy, 4.5f, C_WHITE);
  } else {
    const char *s = s_wt.text;
    if (WF_TEXT_X + gfx::textWidth(FONT_CP26, s) > right) {
      x = gfx::drawText(FONT_CP26, x, base, "…", C_LABEL) + 2;
      while (*s && x + gfx::textWidth(FONT_CP26, s) > right) s++;
    }
    x = gfx::drawText(FONT_CP26, x, base, s, C_WHITE);
  }
  gfx::fillRect((int)x + 2, (int)cy - 13, 2, 27, C_YELLOW);  // cursor
}
static void wifiKeys() {
  KbKey k[KB_MAX];
  const int layer = kbLayer(), n = kbKeys(layer, k);
  for (int i = 0; i < n; i++) {
    float x, y, w;
    kbRect(k[i], x, y, w);
    const float cx = x + w * 0.5f, cy = y + KB_KH * 0.5f;
    switch (k[i].code) {
      case K_SHIFT:
        if (g.wifiShift) gfx::fillRoundRect(x, y, w, KB_KH, 8, C_WHITE);
        else gfx::frameRoundRect(x, y, w, KB_KH, 8, 2, C_OFF, C_BG);
        shiftIcon(cx, cy, g.wifiShift ? C_BG : C_SUB);
        break;
      case K_BACK:
        gfx::frameRoundRect(x, y, w, KB_KH, 8, 2, C_OFF, C_BG);
        backIcon(cx, cy, C_SUB, C_BG);
        break;
      case K_LAYER:
        gfx::frameRoundRect(x, y, w, KB_KH, 8, 2, C_OFF, C_BG);
        textC(FONT_CP22, cx, baseC(FONT_CP22, cy), layer == 2 ? (g.wifiShift ? "ABC" : "abc") : "#+=", C_SUB);
        break;
      case ' ':
        gfx::fillRoundRect(x, y, w, KB_KH, 8, C_OFF);
        textC(FONT_CP18, cx, baseC(FONT_CP18, cy), "space", C_LABEL);
        break;
      default: {
        const char s[2] = { (char)k[i].code, 0 };
        gfx::fillRoundRect(x, y, w, KB_KH, 8, C_OFF);
        textC(FONT_CP26, cx, baseC(FONT_CP26, cy), s, C_WHITE);
        break;
      }
    }
  }
}
static void wifiButtons() {
  const bool ok = kbValid();
  button(KB_OKX, KB_BY, KB_OKW, KB_BH, T26, g.wifiMode == KM_NAME ? "次へ" : "保存して接続", ok ? C_YELLOW : C_OFF, ok ? C_YELLOW : C_BG, ok ? C_BG : C_DOT);
  button(KB_CANX, KB_BY, KB_CANW, KB_BH, T26, "キャンセル", C_LABEL, C_BG, C_WHITE);
}
static void wifiListBody() {
  const WifiPage &w = s_m->wifi;
  const float y0 = (float)(kBodyY - g.scroll), sy = y0 + 32;
  char t[96];
  if (w.connected) {
    gfx::fillCircle(24, sy, 6, C_GREEN);
    snprintf(t, sizeof t, "接続中: %s ・ %s", w.ssid, w.ip[0] ? w.ip : "---");
    const TextSet sets[2] = { T22, T18 };
    mixDrawFit(sets, 2, 40, baseC(FONT_JP22, sy), 584 - 40, t, C_WHITE);
  } else {
    gfx::fillCircle(24, sy, 6, C_DOT);
    mixDraw(T22, 40, baseC(FONT_JP22, sy), "Wi-Fi 未接続", C_SUB);
  }
  mixDraw(T18, 16, y0 + 80, "登録済み（上から順に試します）", C_LABEL);
  const int n = wifiSaved(w);
  for (int i = 0; i < n; i++) {
    const float y = y0 + (float)(WF_ROW0 + i * kRowH), cy = y + kRowH * 0.5f;
    if (y + kRowH <= kBodyY || y >= gfx::H) continue;
    snprintf(t, sizeof t, "%d", i + 1);
    gfx::drawText(FONT_CP22, 20, baseC(FONT_CP22, cy), t, C_LABEL);
    float xr = 584;
    if (w.connected && strcmp(w.saved[i], w.ssid) == 0) {
      const float bw = mixWidth(T18, "接続中") + 28;
      badge(xr - bw, cy - 15, bw, 30, 15, T18, "接続中", C_GREEN, C_BG);
      xr -= bw + 16;
    }
    const TextSet sets[2] = { T28, T22 };
    mixDrawFit(sets, 2, 52, baseC(FONT_JP28, cy), xr - 52, w.saved[i], C_WHITE);
    gfx::fillRect(0, (int)y + kRowH - 1, gfx::W, 1, C_LINE);
  }
  float y = y0 + (float)WF_ROW0;
  if (!n) {
    mixDraw(T22, 20, baseC(FONT_JP22, y + kRowH * 0.5f), "登録したネットワークはありません", C_LABEL);
    gfx::fillRect(0, (int)y + kRowH - 1, gfx::W, 1, C_LINE);
  }
  y += (float)((n ? n : 1) * kRowH);
  const float cy = y + kRowH * 0.5f;
  mixDraw(T28, 20, baseC(FONT_JP28, cy), "＋ ネットワークを追加", n < 8 ? C_YELLOW : C_DOT);
  if (n >= 8) mixDrawR(T18, 584, baseC(FONT_JP18, cy), "8 件まで", C_LABEL);
}
static void wifiScanBody() {
  const WifiPage &w = s_m->wifi;
  const float y0 = (float)(kBodyY - g.scroll);
  const int rows = wifiScanRows(w);
  if (!wifiScanList(w)) {
    const bool busy = w.scanning || !w.scanOk;
    mixDraw(T26, 20, baseC(FONT_JP26, y0 + kRowH * 0.5f), busy ? "検索中…" : "ネットワークが見つかりません", C_SUB);
    gfx::fillRect(0, (int)y0 + kRowH - 1, gfx::W, 1, C_LINE);
  }
  for (int i = 0; i < rows && wifiScanList(w); i++) {
    const WifiFound &f = w.found[i];
    const float y = y0 + (float)(i * kRowH), cy = y + kRowH * 0.5f;
    if (y + kRowH <= kBodyY || y >= WF_HINT_Y) continue;
    signalBars(556, cy + 11, rssiLevel(f.rssi));
    if (f.locked) lockIcon(533, cy, C_SUB);
    float xr = 514;
    if (f.saved) {
      const float bw = mixWidth(T18, "登録済み") + 28;
      badge(xr - bw, cy - 15, bw, 30, 15, T18, "登録済み", C_OFF, C_SUB);
      xr -= bw;
    }
    const TextSet sets[2] = { T28, T22 };
    mixDrawFit(sets, 2, 20, baseC(FONT_JP28, cy), xr - 14 - 20, f.ssid, C_WHITE);
    gfx::fillRect(0, (int)y + kRowH - 1, gfx::W, 1, C_LINE);
  }
  const float cy = y0 + (float)(rows * kRowH) + kRowH * 0.5f;
  const float x = mixDraw(T28, 20, baseC(FONT_JP28, cy), "名前を入力", C_YELLOW);
  mixDraw(T22, x + 8, baseC(FONT_JP28, cy), "（一覧にないとき）", C_LABEL);
}
static void wifiHintFoot() {  // scan view: under the list
  gfx::fillRect(0, WF_HINT_Y, gfx::W, 1, C_OFF);
  mixDraw(T14, 16, (float)WF_HINT_Y + 22, WIFI_HINT[0], C_LABEL);
  mixDraw(T14, 16, (float)WF_HINT_Y + 42, WIFI_HINT[1], C_LABEL);
}
static void wifiResultBody() {
  const Model &m = *s_m;
  const uint8_t st = wifiShown(m);
  const char *title = "接続中…";
  uint16_t c = C_WHITE;
  switch (st) {
    case WJ_OK: title = "接続しました"; break;
    case WJ_BAD_PASSWORD: title = "パスワードが違います"; break;
    case WJ_NOT_FOUND: title = "見つかりません"; break;
    case WJ_FAILED: title = "接続できません"; break;
    default: break;
  }
  if (st == WJ_OK) c = C_GREEN;
  else if (st != WJ_TRYING) c = C_RED;
  mixDrawC(T30, 300, 170, title, c);
  char t[96];
  if (st == WJ_OK) snprintf(t, sizeof t, "%s ・ %s", s_wt.target, m.wifi.ip[0] ? m.wifi.ip : "---");
  else snprintf(t, sizeof t, "%s", s_wt.target);
  const TextSet sets[2] = { T22, T18 };
  drawFitC(sets, 2, 300, 218, 568, t, st == WJ_OK ? C_WHITE : C_SUB);
  if (st == WJ_NOT_FOUND) {
    mixDrawC(T14, 300, 262, WIFI_HINT[0], C_LABEL);
    mixDrawC(T14, 300, 282, WIFI_HINT[1], C_LABEL);
  }
  if (st == WJ_TRYING) return;
  if (wifiRetryable(st)) {
    button(16, WR_BY, 276, WR_BH, T26, "入力しなおす", C_YELLOW, C_YELLOW, C_BG);
    button(308, WR_BY, 276, WR_BH, T26, st == WJ_BAD_PASSWORD ? "やめる" : "OK", C_LABEL, C_BG, C_WHITE);
  } else {
    button(170, WR_BY, 260, WR_BH, T26, "OK", C_YELLOW, C_YELLOW, C_BG);
  }
}
static void wifiDialogDraw() {  // over the list or the scan result (like the maintenance dialog)
  gfx::setAlpha(170);
  gfx::fillRect(0, kBodyY, gfx::W, kBodyH, C_BG);
  gfx::setAlpha(255);
  gfx::frameRoundRect(DLG_X, DLG_Y, DLG_W, DLG_H, 12, 2, C_SUB, C_PANEL);
  const TextSet sets[2] = { T28, T22 };
  drawFitC(sets, 2, 300, DLG_Y + 60, DLG_W - 40, s_wt.dlgSsid, C_WHITE);
  if (g.wifiDialog == WD_FORGET) {
    mixDrawC(T22, 300, DLG_Y + 112, "を削除しますか", C_SUB);
    button(DLG_B1X, DLG_BY, DLG_BW, 72, T26, "削除", C_RED, C_RED, C_WHITE);
  } else {
    mixDrawC(T22, 300, DLG_Y + 112, "パスワードなしのネットワークです。", C_SUB);
    mixDrawC(T22, 300, DLG_Y + 148, "登録しますか", C_SUB);
    button(DLG_B1X, DLG_BY, DLG_BW, 72, T26, "登録する", C_YELLOW, C_YELLOW, C_BG);
  }
  button(DLG_B2X, DLG_BY, DLG_BW, 72, T26, "キャンセル", C_LABEL, C_PANEL, C_WHITE);
}

// ---- scene: item boxes and keys; a changed item is redrawn with everything that overlaps it
enum { WI_HEAD = 0, WI_BODY, WI_HINT, WI_KEYS, WI_BTNS, WI_DLG, WI_COUNT };
struct WifiScene {
  gfx::Rect box[WI_COUNT];
  uint32_t key[WI_COUNT];
};
static WifiScene s_ws;
static void wifiScene(const Model &m, WifiScene &v) {
  const WifiPage &w = m.wifi;
  for (int i = 0; i < WI_COUNT; i++) {
    v.box[i] = gfx::Rect{ 0, 0, 0, 0 };
    v.key[i] = 0;
  }
  const uint32_t k0 = hashU(kHash0, g.wifiView);
  v.box[WI_HEAD] = rectXY(0, 0, gfx::W, kBodyY);
  if (g.wifiView == WV_KEYS) {
    const uint32_t k = hashU(hashU(hashU(k0, g.wifiMode), g.wifiHidden), (uint32_t)s_wt.rev << 8 | s_wt.len);
    v.key[WI_HEAD] = hashStr(k, s_wt.target);
    v.box[WI_KEYS] = rectXY(0, kBodyY, gfx::W, (int)KB_BY - 4);
    v.key[WI_KEYS] = hashU(hashU(k0, (uint32_t)kbLayer()), g.wifiShift);
    v.box[WI_BTNS] = rectXY(0, (int)KB_BY - 4, gfx::W, gfx::H);
    v.key[WI_BTNS] = hashU(hashU(k0, g.wifiMode), kbValid());
  } else {
    v.key[WI_HEAD] = hashU(k0, w.scanning);
    v.box[WI_BODY] = rectXY(0, kBodyY, gfx::W, g.wifiView == WV_SCAN ? WF_HINT_Y : gfx::H);
    uint32_t k = hashU(k0, (uint32_t)g.scroll);
    if (g.wifiView == WV_LIST) {
      k = hashU(hashU(k, w.nSaved), w.connected);
      for (int i = 0; i < wifiSaved(w); i++) k = hashStr(k, w.saved[i]);
      k = hashStr(hashStr(k, w.ssid), w.ip);
    } else if (g.wifiView == WV_SCAN) {
      k = hashU(hashU(hashU(k, w.scanning), w.scanOk), w.nFound);
      for (int i = 0; i < wifiScanRows(w) && wifiScanList(w); i++) {
        const WifiFound &f = w.found[i];
        k = hashU(hashStr(k, f.ssid), (uint32_t)rssiLevel(f.rssi) | (uint32_t)f.locked << 4 | (uint32_t)f.saved << 5);
      }
      v.box[WI_HINT] = rectXY(0, WF_HINT_Y, gfx::W, gfx::H);
      v.key[WI_HINT] = 1;
    } else {
      k = hashU(hashU(k, wifiShown(m)), s_wt.open);
      k = hashStr(hashStr(k, s_wt.target), w.ip);
    }
    v.key[WI_BODY] = k;
  }
  if (g.wifiDialog != WD_NONE) {
    v.box[WI_DLG] = rectXY(0, kBodyY, gfx::W, gfx::H);
    v.key[WI_DLG] = hashStr(hashU(k0, g.wifiDialog), s_wt.dlgSsid);
  }
}
static void wifiItem(int id) {
  switch (id) {
    case WI_HEAD:
      if (g.wifiView == WV_KEYS) wifiField();
      else wifiHeader();
      break;
    case WI_BODY:
      if (g.wifiView == WV_LIST) wifiListBody();
      else if (g.wifiView == WV_SCAN) wifiScanBody();
      else wifiResultBody();
      break;
    case WI_HINT: wifiHintFoot(); break;
    case WI_KEYS: wifiKeys(); break;
    case WI_BTNS: wifiButtons(); break;
    case WI_DLG: wifiDialogDraw(); break;
    default: break;
  }
}
static void wifiRefresh(const WifiScene &v, const gfx::Rect &r) {  // every item inside its own box
  gfx::setClip(r);
  gfx::fillAll(C_BG);
  for (int i = 0; i < WI_COUNT; i++) {
    const gfx::Rect &b = v.box[i];
    if (!overlap(b, r)) continue;
    gfx::setClip(b.x0 > r.x0 ? b.x0 : r.x0, b.y0 > r.y0 ? b.y0 : r.y0, b.x1 < r.x1 ? b.x1 : r.x1, b.y1 < r.y1 ? b.y1 : r.y1);
    wifiItem(i);
  }
  gfx::clearClip();
}
static void wifiFrame(const Model &m, bool full) {
  if (g.scroll < 0) g.scroll = 0;
  if (g.wifiView == WV_RESULT) {  // a result without 入力しなおす is shown: the password copy is not needed
    const uint8_t st = wifiShown(m);
    if (st != WJ_TRYING && !wifiRetryable(st)) wipe(s_wt.retry, sizeof s_wt.retry);
  }
  WifiScene v;
  wifiScene(m, v);
  if (full) {
    wifiRefresh(v, rectXY(0, 0, gfx::W, gfx::H));
    s_ws = v;
    return;
  }
  static RectList inv;
  inv.clear();
  for (int i = 0; i < WI_COUNT; i++) {
    if (v.key[i] == s_ws.key[i] && memcmp(&v.box[i], &s_ws.box[i], sizeof(gfx::Rect)) == 0) continue;
    inv.add(s_ws.box[i]);
    inv.add(v.box[i]);
  }
  for (int i = 0; i < inv.n; i++) wifiRefresh(v, inv.r[i]);
  s_ws = v;
}
static int wifiScrollMax(const Model &m) {
  int h = 0, vis = kBodyH;
  if (g.wifiView == WV_LIST) {
    const int n = wifiSaved(m.wifi);
    h = WF_ROW0 + (n ? n : 1) * kRowH + kRowH;
  } else if (g.wifiView == WV_SCAN) {
    h = (wifiScanRows(m.wifi) + 1) * kRowH;
    vis = WF_HINT_Y - kBodyY;
  }
  return h > vis ? h - vis : 0;
}

// ---- taps
static void wifiOpenKeys(uint8_t mode, const char *ssid) {  // an empty keyboard, lower case, text shown
  g.wifiView = WV_KEYS;
  g.wifiMode = mode;
  g.wifiShift = g.wifiSym = g.wifiHidden = false;
  wipe(s_wt.text, sizeof s_wt.text);
  s_wt.len = 0;
  s_wt.rev++;
  if (ssid != s_wt.target) copyText(s_wt.target, sizeof s_wt.target, ssid);
}
static void wifiToList() {
  wipe(s_wt.retry, sizeof s_wt.retry);
  g.wifiView = WV_LIST;
  g.scroll = 0;
}
static void wifiSave(const char *pass, ActionQueue &out) {  // ACT_WIFI_SAVE(target, pass) -> result view
  Action a;
  a.kind = ACT_WIFI_SAVE;
  copyText(a.ssid, sizeof a.ssid, s_wt.target);
  copyText(a.pass, sizeof a.pass, pass);
  const bool ok = out.push(a);
  wipe(a.pass, sizeof a.pass);
  if (!ok) return;  // queue full: nothing sent, the keyboard stays
  g.wifiView = WV_RESULT;
}
static void kbTap(int x, int y) {
  KbKey k[KB_MAX];
  const int n = kbKeys(kbLayer(), k);
  for (int i = 0; i < n; i++) {
    float kx, ky, kw;
    kbRect(k[i], kx, ky, kw);
    const float hx = KB_GAP * 0.5f, hy = (KB_PY - KB_KH) * 0.5f;  // the gaps belong to the nearest key
    if (!inBox(x, y, kx - hx, ky - hy, kw + 2 * hx, KB_KH + 2 * hy)) continue;
    switch (k[i].code) {
      case K_SHIFT: g.wifiShift = !g.wifiShift; break;
      case K_LAYER: g.wifiSym = !g.wifiSym; break;
      case K_BACK:
        if (s_wt.len) {
          s_wt.text[--s_wt.len] = 0;
          s_wt.rev++;
        }
        break;
      default:
        if (s_wt.len < (g.wifiMode == KM_NAME ? 32 : 64)) {
          s_wt.text[s_wt.len++] = (char)k[i].code;
          s_wt.text[s_wt.len] = 0;
          s_wt.rev++;
        }
        break;
    }
    return;
  }
}
static void wifiTap(const Model &m, int x, int y, ActionQueue &out) {
  const WifiPage &w = m.wifi;
  Action a;
  if (g.wifiDialog != WD_NONE) {  // modal: only its two buttons
    if (inBox(x, y, DLG_B1X, DLG_BY, DLG_BW, 72)) {
      if (g.wifiDialog == WD_FORGET) {
        a.kind = ACT_WIFI_FORGET;
        copyText(a.ssid, sizeof a.ssid, s_wt.dlgSsid);
        out.push(a);
      } else {
        copyText(s_wt.target, sizeof s_wt.target, s_wt.dlgSsid);
        wipe(s_wt.retry, sizeof s_wt.retry);
        s_wt.open = true;
        wifiSave("", out);
      }
      g.wifiDialog = WD_NONE;
    } else if (inBox(x, y, DLG_B2X, DLG_BY, DLG_BW, 72)) {
      g.wifiDialog = WD_NONE;
    }
    return;
  }
  if (g.wifiView == WV_KEYS) {
    if (y < kBodyY) {
      if ((float)x >= WF_SHOW_X - 8 && g.wifiMode == KM_PASS) g.wifiHidden = !g.wifiHidden;
      return;
    }
    if ((float)y < KB_BY - 4) {
      kbTap(x, y);
      return;
    }
    if ((float)x >= KB_CANX - 8) {  // キャンセル
      wipe(s_wt.text, sizeof s_wt.text);
      s_wt.len = 0;
      s_wt.rev++;
      g.wifiView = WV_SCAN;
      g.scroll = 0;
      return;
    }
    if ((float)x >= KB_OKX + KB_OKW + 8 || !kbValid()) return;
    if (g.wifiMode == KM_NAME) {  // 次へ: the password of the typed network
      copyText(s_wt.target, sizeof s_wt.target, s_wt.text);
      wifiOpenKeys(KM_PASS, s_wt.target);
      return;
    }
    s_wt.open = false;
    memcpy(s_wt.retry, s_wt.text, sizeof s_wt.retry);
    wifiSave(s_wt.text, out);
    if (g.wifiView != WV_RESULT) {
      wipe(s_wt.retry, sizeof s_wt.retry);
      return;
    }
    wipe(s_wt.text, sizeof s_wt.text);
    s_wt.len = 0;
    s_wt.rev++;
    return;
  }
  if (y < kBodyY) {
    if (x < WF_BACK_W) {
      if (g.wifiView == WV_SCAN) {
        g.wifiView = WV_LIST;
        g.scroll = 0;
      } else {
        a.kind = ACT_PAGE;
        a.arg = SC_SETTINGS;
        out.push(a);
      }
    } else if (g.wifiView == WV_SCAN && (float)x >= WF_RESCAN_X - 12 && !w.scanning) {
      a.kind = ACT_WIFI_SCAN;
      out.push(a);
    }
    return;
  }
  const int cy = y - kBodyY + g.scroll;
  if (g.wifiView == WV_LIST) {
    const int n = wifiSaved(w), add = WF_ROW0 + (n ? n : 1) * kRowH;
    if (cy >= WF_ROW0 && cy < WF_ROW0 + n * kRowH) {
      copyText(s_wt.dlgSsid, sizeof s_wt.dlgSsid, w.saved[(cy - WF_ROW0) / kRowH]);
      g.wifiDialog = WD_FORGET;
    } else if (cy >= add && cy < add + kRowH && n < 8) {
      a.kind = ACT_WIFI_SCAN;
      out.push(a);
      g.wifiView = WV_SCAN;
      g.scroll = 0;
    }
  } else if (g.wifiView == WV_SCAN) {
    if (y >= WF_HINT_Y) return;
    const int i = cy / kRowH, rows = wifiScanRows(w);
    if (i < rows && wifiScanList(w)) {
      const WifiFound &f = w.found[i];
      if (f.locked) {
        wifiOpenKeys(KM_PASS, f.ssid);
      } else {
        copyText(s_wt.dlgSsid, sizeof s_wt.dlgSsid, f.ssid);
        g.wifiDialog = WD_OPEN;
      }
    } else if (i == rows) {
      wifiOpenKeys(KM_NAME, "");
    }
  } else {  // result
    const uint8_t st = wifiShown(m);
    if (st == WJ_TRYING || y < WR_BY || y >= WR_BY + WR_BH) return;
    if (wifiRetryable(st)) {
      if (x < 300) {  // 入力しなおす: the keyboard with the previous text
        wifiOpenKeys(KM_PASS, s_wt.target);
        memcpy(s_wt.text, s_wt.retry, sizeof s_wt.text);
        s_wt.len = (uint8_t)strlen(s_wt.text);
        wipe(s_wt.retry, sizeof s_wt.retry);
      } else {
        wifiToList();
      }
    } else if (x >= 170 && x < 430) {
      wifiToList();
    }
  }
}

// ================================================================ 走行記録 (settings > 走行記録)
// Round gauge 2.15.0-2.15.3 (ui.cpp updOdo): one period at a time: the period, the odometer at its end (now for
// the current one) large, start 〜 end, the distance and ▲ the over-rpm time. 600x450 and touch only: the levels
// are tabs (1 年 … 1 日; a finer tab = the last period inside the one shown, a coarser tab = the one that holds
// it), back / forward are buttons and swipes (finger to the right = earlier). Before the date is known:
// 時刻 待ち with the odometer now and the distance since power-up.
static const float OD_TAB_X = 16, OD_TAB_Y = 78, OD_TAB_W = 108, OD_TAB_H = 52, OD_TAB_GAP = 6;
static const float OD_BTN_Y = 378, OD_BTN_H = 64, OD_BTN_W = 200, OD_BTN_PREV_X = 16, OD_BTN_NEXT_X = 384;
static const char *const OD_NAME[OL_COUNT] = { "1 年", "3 ヶ月", "1 ヶ月", "1 週", "1 日" };
static const char *const OD_NOW[OL_COUNT] = { "今年", "", "今月", "今週", "今日" };
static const char *const OD_WDAY[7] = { "日", "月", "火", "水", "木", "金", "土" };
static float odTabX(int i) {
  return OD_TAB_X + (float)i * (OD_TAB_W + OD_TAB_GAP);
}
static void odoPeriodText(const OdoPage &o, char *out, size_t n) {
  const OdoDate &a = o.first, &b = o.last;
  const char *now = o.current ? OD_NOW[o.level < OL_COUNT ? o.level : 0] : "";
  const char *sp = now[0] ? "  " : "";
  switch (o.level) {
    case OL_YEAR: snprintf(out, n, "%u 年%s%s", (unsigned)a.y, sp, now); break;
    case OL_QUARTER: snprintf(out, n, "%u 年 %u〜%u 月", (unsigned)a.y, (unsigned)a.m, (unsigned)b.m); break;
    case OL_MONTH: snprintf(out, n, "%u 年 %u 月%s%s", (unsigned)a.y, (unsigned)a.m, sp, now); break;
    case OL_WEEK: snprintf(out, n, "%u/%u〜%u/%u%s%s", (unsigned)a.m, (unsigned)a.d, (unsigned)b.m, (unsigned)b.d, sp, now); break;
    default:
      snprintf(out, n, "%u/%u/%u（%s）%s%s", (unsigned)a.y, (unsigned)a.m, (unsigned)a.d, OD_WDAY[a.wday % 7], sp, now);
      break;
  }
}
static void fmtHms(char *o, size_t n, uint32_t s) {  // h:mm:ss
  if (s > 359999u) s = 359999u;
  snprintf(o, n, "%u:%02u:%02u", (unsigned)(s / 3600u), (unsigned)(s / 60u % 60u), (unsigned)(s % 60u));
}
static void odoBigValue(float base, const char *num) {  // centred: number (64 px) + " km"
  const float wn = gfx::textWidth(FONT_NUM64, num), wu = mixWidth(T28, "km");
  const float x = 300 - (wn + 10 + wu) * 0.5f;
  gfx::drawText(FONT_NUM64, x, base, num, C_WHITE);
  mixDraw(T28, x + wn + 10, base, "km", C_LABEL);
}
static void odoHeader() {
  const Model &m = *s_m;
  const PageStyle &st = pageStyle(SC_ODO);
  drawHeaderBand(st);
  const float cy = 32;
  float x = mixDraw(T22, 30, baseC(FONT_JP22, cy), "‹ 設定", C_YELLOW);
  mixDraw(T30, x + 22, baseC(FONT_JP30, cy), "走行記録", st.color);
  char t[48], n[16];
  if (!m.odo.storageOk) {
    mixDrawR(T18, 584, baseC(FONT_JP18, cy), "保存領域なし", C_RED);
  } else {
    fmtGrouped(n, sizeof n, m.odo.records);
    snprintf(t, sizeof t, "記録 %s 件", n);
    mixDrawR(T18, 584, baseC(FONT_JP18, cy), t, C_LABEL);
  }
}
static void odoBody() {
  const OdoPage &o = s_m->odo;
  char t[64], a[16], b[16];
  // level tabs: the shown one yellow; without a date only the shown one is lit
  for (int i = 0; i < OL_COUNT; i++) {
    const bool cur = i == o.level, can = o.dateOk;
    const uint16_t border = cur ? C_YELLOW : (can ? C_LABEL : C_OFF);
    const uint16_t fg = cur ? C_YELLOW : (can ? C_WHITE : C_DOT);
    button(odTabX(i), OD_TAB_Y, OD_TAB_W, OD_TAB_H, T22, OD_NAME[i], border, C_BG, fg);
  }
  if (!o.dateOk) {  // the date is not known yet (GNSS / RTC): no period; the odometer now and since power-up
    mixDrawC(T30, 300, baseC(FONT_JP30, 176), "時刻 待ち", C_SUB);
    if (o.curOk) {
      fmtGrouped(a, sizeof a, o.cur);
      odoBigValue(262, a);
      snprintf(t, sizeof t, "起動から %u km", (unsigned)o.sinceBootKm);
      mixDrawC(T26, 300, baseC(FONT_JP26, 310), t, C_LABEL);
    } else {
      mixDrawC(T28, 300, baseC(FONT_JP28, 250), "ODO 受信待ち", C_LABEL);
    }
  } else {
    odoPeriodText(o, t, sizeof t);
    mixDrawC(T30, 300, baseC(FONT_JP30, 166), t, C_SUB);
    if (!o.odoKnown) {
      mixDrawC(T30, 300, baseC(FONT_JP30, 262), "記録なし", C_LABEL);
    } else {
      fmtGrouped(a, sizeof a, o.odoEnd);
      odoBigValue(252, a);  // the odometer at the end of the period (now for the current one)
      fmtGrouped(a, sizeof a, o.odoStart);
      fmtGrouped(b, sizeof b, o.odoEnd);
      snprintf(t, sizeof t, "%s 〜 %s", a, b);
      mixDrawC(T26, 300, baseC(FONT_JP26, 286), t, C_LABEL);
      // distance | ▲ over-rpm time (two columns)
      mixDrawC(T18, 150, baseC(FONT_JP18, 312), "距離", C_LABEL);
      mixDrawC(T18, 450, baseC(FONT_JP18, 312), "過走行（3000 rpm 以上）", C_LABEL);
      fmtGrouped(a, sizeof a, o.km);
      const float wn = gfx::textWidth(FONT_NUM48, a), wu = mixWidth(T22, "km");
      float x = 150 - (wn + 8 + wu) * 0.5f;
      gfx::drawText(FONT_NUM48, x, 362, a, C_WHITE);
      mixDraw(T22, x + wn + 8, 362, "km", C_LABEL);
      fmtHms(b, sizeof b, o.aboveS);
      const float wt = gfx::textWidth(FONT_NUM48, b);
      x = 450 - (wt + 30) * 0.5f;
      gfx::Pt tri[3] = { { x + 11, 340 }, { x + 22, 360 }, { x, 360 } };  // ▲ in the over-rpm colour
      gfx::fillPoly(tri, 3, C_YELLOW);
      gfx::drawText(FONT_NUM48, x + 30, 362, b, C_YELLOW);
      gfx::fillRect(299, 306, 2, 60, C_OFF);
    }
  }
  const bool back = o.dateOk && o.canBack, fwd = o.dateOk && o.canFwd;
  button(OD_BTN_PREV_X, OD_BTN_Y, OD_BTN_W, OD_BTN_H, T26, "‹ 前", back ? C_LABEL : C_OFF, C_BG, back ? C_WHITE : C_DOT);
  button(OD_BTN_NEXT_X, OD_BTN_Y, OD_BTN_W, OD_BTN_H, T26, "次 ›", fwd ? C_LABEL : C_OFF, C_BG, fwd ? C_WHITE : C_DOT);
}
static void odoFrame(const Model &m, bool full) {
  const uint32_t kh = hashU(hashU(kHash0, m.odo.storageOk), m.odo.records);
  const uint32_t kb = hashBytes(kHash0, &m.odo, sizeof m.odo);
  if (full || kh != s_keyHead) redraw(rectXY(0, 0, gfx::W, kBodyY), odoHeader);
  if (full || kb != s_keyBody) redraw(rectXY(0, kBodyY, gfx::W, gfx::H), odoBody);
  s_keyHead = kh;
  s_keyBody = kb;
}
static void odoMove(uint8_t mv, uint8_t level, ActionQueue &out) {
  Action a;
  a.kind = ACT_ODO;
  a.arg = mv;
  a.value = level;
  out.push(a);
}
static void odoTap(const Model &m, int x, int y, ActionQueue &out) {
  const OdoPage &o = m.odo;
  if (y < kBodyY) {
    if (x < 150) {
      Action a;
      a.kind = ACT_PAGE;
      a.arg = SC_SETTINGS;
      out.push(a);
    }
    return;
  }
  for (int i = 0; i < OL_COUNT; i++)
    if (inBox(x, y, odTabX(i), OD_TAB_Y, OD_TAB_W, OD_TAB_H)) {
      if (o.dateOk && i != o.level) odoMove(OM_LEVEL, (uint8_t)i, out);
      return;
    }
  if (inBox(x, y, OD_BTN_PREV_X, OD_BTN_Y, OD_BTN_W, OD_BTN_H)) {
    if (o.dateOk && o.canBack) odoMove(OM_PREV, 0, out);
  } else if (inBox(x, y, OD_BTN_NEXT_X, OD_BTN_Y, OD_BTN_W, OD_BTN_H)) {
    if (o.dateOk && o.canFwd) odoMove(OM_NEXT, 0, out);
  }
}
void odoSwipe(const Model &m, int dx, ActionQueue &out) {  // finger to the right = earlier period
  const OdoPage &o = m.odo;
  if (!o.dateOk) return;
  if (dx > 0 && o.canBack) odoMove(OM_PREV, 0, out);
  else if (dx < 0 && o.canFwd) odoMove(OM_NEXT, 0, out);
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
  if (m.screen == SC_WIFI) {
    wifiFrame(m, full);
    return;
  }
  if (m.screen == SC_ODO) {
    odoFrame(m, full);
    return;
  }
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
    case SC_WIFI: return wifiScrollMax(m);
    default: return 0;
  }
  return h > kBodyH ? h - kBodyH : 0;
}

bool pageModal() {
  if (g.shown == SC_WIFI) return g.wifiDialog != WD_NONE || g.wifiView == WV_KEYS || g.wifiView == WV_RESULT;
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
    case SC_WIFI: wifiTap(m, x, y, out); break;
    case SC_ODO: odoTap(m, x, y, out); break;
    default: break;
  }
}

}  // namespace in

bool testWifiKey(uint8_t layer, uint32_t code, int16_t *x, int16_t *y) {
  using namespace in;
  float bx = 0, by = 0, bw = 0, bh = KB_KH;
  if (code == TK_SHOW) {
    bx = WF_SHOW_X;
    by = WF_SHOW_Y;
    bw = WF_SHOW_W;
    bh = WF_SHOW_H;
  } else if (code == TK_OK || code == TK_CANCEL) {
    bx = code == TK_OK ? KB_OKX : KB_CANX;
    by = KB_BY;
    bw = code == TK_OK ? KB_OKW : KB_CANW;
    bh = KB_BH;
  } else {
    uint32_t want = 0;
    if (code == TK_SHIFT) want = K_SHIFT;
    else if (code == TK_LAYER) want = K_LAYER;
    else if (code == TK_BACK) want = K_BACK;
    else if (code >= 0x20 && code < 0x7F) want = code;
    if (!want || layer > 2) return false;
    KbKey k[KB_MAX];
    const int n = kbKeys(layer, k);
    int hit = -1;
    for (int i = 0; i < n && hit < 0; i++)
      if (k[i].code == want) hit = i;
    if (hit < 0) return false;
    kbRect(k[hit], bx, by, bw);
  }
  *x = (int16_t)lroundf(bx + bw * 0.5f);
  *y = (int16_t)lroundf(by + bh * 0.5f);
  return true;
}

}  // namespace ui
