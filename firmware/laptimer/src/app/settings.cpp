// settings.cpp - see settings.h
#include "settings.h"

#include <string.h>

#include "../hw/nvs.h"

namespace settings {

static constexpr uint16_t kVersion = 2;  // 2: odometer and track time as exact integers
static const char* const kKey = "settings";

void defaults(Settings* s) {
  memset(s, 0, sizeof *s);
  s->version = kVersion;
  s->shiftGreen = 4500;
  s->shiftYellow = 5500;
  s->shiftRed = 6000;
  s->shiftFlash = 6300;
  s->brightnessPct = 100;
  s->illumPct = 40;
  s->tzMin = 540;
  s->oilWarmBit = 1;
  s->bleOn = 1;
  s->canRawLog = 0;
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

}  // namespace settings
