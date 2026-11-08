/*
 * laptimer.ino - 595_laptimer: lap timer and gauge for the Abarth 595
 * (Waveshare ESP32-S3-Touch-AMOLED-2.41 + Matek M9N-5883 + XTSD 2 GB + the round gauge's CAN reader box).
 *
 * Build and flash only with ./tools/go.sh flash (it embeds the web pages and default settings and passes the
 * core/ and ui/ folders as libraries). Board settings are fixed there:
 *   ESP32S3 Dev Module, PSRAM OPI, Flash 16 MB QIO, Partition "16M Flash (3MB APP/9.9MB FATFS)",
 *   USB CDC On Boot Enabled, arduino-esp32 3.3.x, NimBLE-Arduino 1.4.3.
 * Everything starts in app::start(); the tasks are described in src/app/app.cpp.
 */
#include "src/app/app.h"

void setup() { app::start(); }

// Nothing runs in loop(): ending its task gives its 8 KB stack back to the internal RAM (Wi-Fi and BLE need it)
void loop() { vTaskDelete(nullptr); }
