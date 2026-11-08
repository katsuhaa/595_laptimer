// tools/host/report_build.h - the report file of a session folder, made the way the device makes it
// (web module /report): web/report.html cut at the marker, and between the two parts the data blocks
// of core/reportdata (meta.json, laps CSV, the main log decimated with lt::report::LogFilter, info),
// the main log read in 4 KB pieces through the same small buffers as on the device.
#pragma once

#include <string>
#include <vector>

#include "core/reportdata.h"

namespace rb {

struct Template {
  std::string head;  // report.html before the marker
  std::string tail;  // after it
};
// Reads web/report.html; FAIL unless the marker is there exactly once.
bool loadTemplate(const std::string& path, Template* t, std::string* err);

struct Data {
  std::string blocks;  // the four script blocks, in order
  lt::report::FilterStats log;
  std::string metaName, lapsName, mainName;  // files used ("" when missing)
};
// The data blocks for the session folder dir.
bool dataBlocks(const std::string& dir, const std::string& generator, const std::vector<std::string>& notes,
                Data* d, std::string* err);

// Pulls a block's text back out of a report file (for the checks); false when missing or twice.
bool blockText(const std::string& html, const char* open, std::string* text);

}  // namespace rb
