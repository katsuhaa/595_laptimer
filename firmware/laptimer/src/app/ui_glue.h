// ui_glue.h - fills ui::Model from the app state, feeds touch to the UI, applies its actions and pushes the
// changed rectangles to the panel (UI task).
#pragma once
#include <stddef.h>

namespace uiglue {

void begin(bool lcdOk);  // framebuffers (PSRAM), first frame (self-test screen)
void task(void*);
// Also used by the web module (hooks)
bool maintDone(const char* id);
bool courseFromVbo(const char* tmpPath, char* err, int errCap);

}  // namespace uiglue
