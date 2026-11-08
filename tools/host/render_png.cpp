// tools/host/render_png.cpp - see render_png.h
#include "render_png.h"

#include <stdio.h>

#include <vector>

namespace render {

static uint32_t crcTable[256];
static bool crcReady = false;
static void crcInit() {
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crcTable[n] = c;
  }
  crcReady = true;
}
static uint32_t crc32(const uint8_t *p, size_t n, uint32_t c = 0xFFFFFFFFu) {
  if (!crcReady) crcInit();
  for (size_t i = 0; i < n; i++) c = crcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c;
}

static void put32(std::vector<uint8_t> &v, uint32_t x) {
  v.push_back((uint8_t)(x >> 24));
  v.push_back((uint8_t)(x >> 16));
  v.push_back((uint8_t)(x >> 8));
  v.push_back((uint8_t)x);
}
static void chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data) {
  put32(out, (uint32_t)data.size());
  std::vector<uint8_t> td(type, type + 4);
  td.insert(td.end(), data.begin(), data.end());
  out.insert(out.end(), td.begin(), td.end());
  put32(out, crc32(td.data(), td.size()) ^ 0xFFFFFFFFu);
}

bool writePng565(const std::string &path, const uint16_t *px, int w, int h) {
  // raw scanlines: filter byte 0 + RGB
  std::vector<uint8_t> raw;
  raw.reserve((size_t)h * (size_t)(w * 3 + 1));
  for (int y = 0; y < h; y++) {
    raw.push_back(0);
    for (int x = 0; x < w; x++) {
      const uint16_t c = px[y * w + x];
      raw.push_back((uint8_t)((((c >> 11) & 31) * 255 + 15) / 31));
      raw.push_back((uint8_t)((((c >> 5) & 63) * 255 + 31) / 63));
      raw.push_back((uint8_t)(((c & 31) * 255 + 15) / 31));
    }
  }
  // zlib stream with stored blocks (max 65535 bytes each)
  std::vector<uint8_t> z;
  z.push_back(0x78);
  z.push_back(0x01);
  size_t pos = 0;
  while (pos < raw.size() || raw.empty()) {
    const size_t len = raw.size() - pos > 65535 ? 65535 : raw.size() - pos;
    const bool last = pos + len >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back((uint8_t)len);
    z.push_back((uint8_t)(len >> 8));
    z.push_back((uint8_t)~len);
    z.push_back((uint8_t)(~len >> 8));
    z.insert(z.end(), raw.begin() + (long)pos, raw.begin() + (long)(pos + len));
    pos += len;
    if (raw.empty()) break;
  }
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) {
    a = (a + v) % 65521u;
    b = (b + a) % 65521u;
  }
  put32(z, (b << 16) | a);

  std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
  std::vector<uint8_t> ihdr;
  put32(ihdr, (uint32_t)w);
  put32(ihdr, (uint32_t)h);
  ihdr.push_back(8);  // bit depth
  ihdr.push_back(2);  // colour type RGB
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);
  chunk(png, "IHDR", ihdr);
  chunk(png, "IDAT", z);
  chunk(png, "IEND", std::vector<uint8_t>());
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
  return fclose(f) == 0 && ok;
}

}  // namespace render
