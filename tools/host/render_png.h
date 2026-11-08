// tools/host/render_png.h - tiny self-contained PNG writer for the UI render tool (RGB565 -> 8-bit RGB PNG).
// Stored (uncompressed) deflate blocks, CRC-32 and Adler-32 computed here; no zlib needed.
#pragma once
#include <stdint.h>

#include <string>

namespace render {

// w x h RGB565 pixels -> PNG file. Returns false when the file cannot be written.
bool writePng565(const std::string &path, const uint16_t *px, int w, int h);

}  // namespace render
