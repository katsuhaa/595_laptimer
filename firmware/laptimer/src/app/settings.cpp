// settings.cpp - see settings.h
#include "settings.h"

#include <string.h>

#include "../hw/nvs.h"

namespace settings {

static constexpr uint16_t kVersion = 3;  // 2: odometer and track time as exact integers, 3: round gauge settings
static const char* const kKey = "settings";
static constexpr uint16_t kTimesVersion = 1;
static const char* const kTimesKey = "times";

void defaults(Settings* s) {
  memset(s, 0, sizeof *s);
  s->version = kVersion;
  s->shiftGreen = 4500;
  s->shiftYellow = 5500;
  s->shiftRed = 6000;
  s->shiftFlash = 6300;
  s->brightnessPct = 80;  // round gauge SET_BRIGHT_DEFAULT
  s->illumPct = 30;       // round gauge SET_ILLUM_DEFAULT
  s->tzMin = 540;
  s->oilWarmBit = 1;
  s->bleOn = 1;
  s->canRawLog = 0;
  // round gauge config.h SET_*_DEFAULT
  s->shiftRpm = 5800;
  s->ringOne = 0;
  s->autoReturnMin = 3;
  s->warnTempC = 110;
  s->fuelWarnPct = 20;
  s->loadRpm = 3000;
  s->loadYellowMin = 10;
  s->loadRedMin = 30;
  s->loadResetMin = 5;
  s->maintHours = 5;
  s->demoOn = 0;
}

bool load(Settings* s) {
  Settings t;
  if (hw::nvsLoad(kKey, &t, sizeof t) && t.version == kVersion) {
    *s = t;
    return true;
  }
  defaults(s);
  return false;
}

bool save(const Settings& s) { return hw::nvsSave(kKey, &s, sizeof s); }

bool loadTimes(Times* t) {
  Times x;
  if (hw::nvsLoad(kTimesKey, &x, sizeof x) && x.version == kTimesVersion) {
    *t = x;
    return true;
  }
  memset(t, 0, sizeof *t);
  t->version = kTimesVersion;
  return false;
}

bool saveTimes(const Times& t) {
  Times x = t;
  x.version = kTimesVersion;
  return hw::nvsSave(kTimesKey, &x, sizeof x);
}

}  // namespace settings
