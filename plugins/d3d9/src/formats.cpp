#include "formats.h"

#include <remix/remix_native_ext.h>

#include <cstring>

namespace openremix::d3d9 {
namespace {

void rgb565(uint16_t c, uint8_t out[3]) {
  out[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
  out[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
  out[2] = uint8_t((c & 31) * 255 / 31);
}

// Decodes the 8-byte colour part of a BC1/BC2/BC3 block into 16 RGBA pixels.
// `bc1Alpha` enables BC1's 3-colour mode with transparent black.
void colorBlock(const uint8_t *block, bool bc1Alpha, uint8_t pixels[16][4]) {
  uint16_t c0, c1;
  std::memcpy(&c0, block, 2);
  std::memcpy(&c1, block + 2, 2);
  uint8_t palette[4][4];
  rgb565(c0, palette[0]);
  rgb565(c1, palette[1]);
  for (int i = 0; i < 4; ++i)
    palette[i][3] = 255;
  if (c0 > c1 || !bc1Alpha) {
    for (int k = 0; k < 3; ++k) {
      palette[2][k] = uint8_t((2 * palette[0][k] + palette[1][k]) / 3);
      palette[3][k] = uint8_t((palette[0][k] + 2 * palette[1][k]) / 3);
    }
  } else {
    for (int k = 0; k < 3; ++k) {
      palette[2][k] = uint8_t((palette[0][k] + palette[1][k]) / 2);
      palette[3][k] = 0;
    }
    palette[3][3] = 0;
  }
  uint32_t bits;
  std::memcpy(&bits, block + 4, 4);
  for (int i = 0; i < 16; ++i)
    std::memcpy(pixels[i], palette[(bits >> (2 * i)) & 3], 4);
}

template <class Decode>
std::vector<uint8_t> decodeBlocks(const uint8_t *blocks, uint32_t width, uint32_t height, uint32_t blockBytes,
                                  Decode fn) {
  std::vector<uint8_t> out(size_t(width) * height * 4);
  const uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
  uint8_t pixels[16][4];
  for (uint32_t by = 0; by < bh; ++by)
    for (uint32_t bx = 0; bx < bw; ++bx) {
      fn(blocks + (size_t(by) * bw + bx) * blockBytes, pixels);
      for (uint32_t y = 0; y < 4 && by * 4 + y < height; ++y)
        for (uint32_t x = 0; x < 4 && bx * 4 + x < width; ++x)
          std::memcpy(&out[((size_t(by) * 4 + y) * width + bx * 4 + x) * 4], pixels[y * 4 + x], 4);
    }
  return out;
}

uint8_t expand(uint32_t value, uint32_t bits) { return uint8_t(value * 255 / ((1u << bits) - 1)); }

// One pixel of an uncompressed format to RGBA8.
bool pixel(D3DFORMAT format, const uint8_t *p, uint8_t out[4]) {
  uint16_t v16;
  switch (format) {
  case D3DFMT_A8R8G8B8: out[0] = p[2], out[1] = p[1], out[2] = p[0], out[3] = p[3]; return true;
  case D3DFMT_X8R8G8B8: out[0] = p[2], out[1] = p[1], out[2] = p[0], out[3] = 255; return true;
  case D3DFMT_A8B8G8R8: std::memcpy(out, p, 4); return true;
  case D3DFMT_X8B8G8R8: out[0] = p[0], out[1] = p[1], out[2] = p[2], out[3] = 255; return true;
  case D3DFMT_R8G8B8: out[0] = p[2], out[1] = p[1], out[2] = p[0], out[3] = 255; return true;
  case D3DFMT_R5G6B5:
    std::memcpy(&v16, p, 2);
    rgb565(v16, out);
    out[3] = 255;
    return true;
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5:
    std::memcpy(&v16, p, 2);
    out[0] = expand((v16 >> 10) & 31, 5), out[1] = expand((v16 >> 5) & 31, 5), out[2] = expand(v16 & 31, 5);
    out[3] = format == D3DFMT_A1R5G5B5 && !(v16 & 0x8000) ? 0 : 255;
    return true;
  case D3DFMT_A4R4G4B4:
  case D3DFMT_X4R4G4B4:
    std::memcpy(&v16, p, 2);
    out[0] = expand((v16 >> 8) & 15, 4), out[1] = expand((v16 >> 4) & 15, 4), out[2] = expand(v16 & 15, 4);
    out[3] = format == D3DFMT_A4R4G4B4 ? expand(v16 >> 12, 4) : 255;
    return true;
  case D3DFMT_A2R10G10B10: {
    uint32_t v;
    std::memcpy(&v, p, 4);
    out[0] = uint8_t(((v >> 20) & 1023) >> 2), out[1] = uint8_t(((v >> 10) & 1023) >> 2),
    out[2] = uint8_t((v & 1023) >> 2), out[3] = expand(v >> 30, 2);
    return true;
  }
  case D3DFMT_L8: out[0] = out[1] = out[2] = p[0], out[3] = 255; return true;
  case D3DFMT_A8: out[0] = out[1] = out[2] = 255, out[3] = p[0]; return true;
  case D3DFMT_A8L8: out[0] = out[1] = out[2] = p[0], out[3] = p[1]; return true;
  case D3DFMT_A4L4: out[0] = out[1] = out[2] = expand(p[0] & 15, 4), out[3] = expand(p[0] >> 4, 4); return true;
  case D3DFMT_L16:
    std::memcpy(&v16, p, 2);
    out[0] = out[1] = out[2] = uint8_t(v16 >> 8), out[3] = 255;
    return true;
  default: return false;
  }
}

} // namespace

bool formatLayout(D3DFORMAT format, FormatLayout &out) {
  switch (format) {
  case D3DFMT_DXT1: out = {8, true}; return true;
  case D3DFMT_DXT2:
  case D3DFMT_DXT3:
  case D3DFMT_DXT4:
  case D3DFMT_DXT5: out = {16, true}; return true;
  case D3DFMT_A8R8G8B8:
  case D3DFMT_X8R8G8B8:
  case D3DFMT_A8B8G8R8:
  case D3DFMT_X8B8G8R8:
  case D3DFMT_A2R10G10B10: out = {4, false}; return true;
  case D3DFMT_R8G8B8: out = {3, false}; return true;
  case D3DFMT_R5G6B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5:
  case D3DFMT_A4R4G4B4:
  case D3DFMT_X4R4G4B4:
  case D3DFMT_A8L8:
  case D3DFMT_L16: out = {2, false}; return true;
  case D3DFMT_L8:
  case D3DFMT_A8:
  case D3DFMT_A4L4: out = {1, false}; return true;
  default: return false;
  }
}

uint32_t rowBytes(const FormatLayout &layout, uint32_t width) {
  return layout.compressed ? ((width + 3) / 4) * layout.blockBytes : width * layout.blockBytes;
}

uint32_t rowCount(const FormatLayout &layout, uint32_t height) {
  return layout.compressed ? (height + 3) / 4 : height;
}

std::vector<uint8_t> packLevel(D3DFORMAT format, uint32_t width, uint32_t height, const uint8_t *data,
                               uint32_t pitch) {
  FormatLayout layout;
  if (!formatLayout(format, layout) || !data)
    return {};
  const uint32_t bytes = rowBytes(layout, width), packed = (bytes + 3) & ~3u, rows = rowCount(layout, height);
  std::vector<uint8_t> out(size_t(packed) * rows, 0);
  for (uint32_t r = 0; r < rows; ++r)
    std::memcpy(out.data() + size_t(r) * packed, data + size_t(r) * pitch, bytes);
  return out;
}

bool convertLevel(D3DFORMAT format, uint32_t width, uint32_t height, const uint8_t *packed, bool srgb,
                  Converted &out) {
  FormatLayout layout;
  if (!formatLayout(format, layout) || !packed || !width || !height)
    return false;
  out = Converted();
  const size_t blocks = size_t(rowBytes(layout, width)) * rowCount(layout, height);
  switch (format) {
  case D3DFMT_DXT1:
    // BC1 with one-bit alpha (its 3-colour mode's transparent texels).
    out.data.assign(packed, packed + blocks);
    out.format = srgb ? REMIXAPI_FORMAT_BC1_RGBA_SRGB_EXT : REMIXAPI_FORMAT_BC1_RGBA_UNORM_EXT;
    out.rgba8 = false;
    return true;
  case D3DFMT_DXT4:
  case D3DFMT_DXT5:
    out.data.assign(packed, packed + blocks);
    out.format = srgb ? REMIXAPI_FORMAT_BC3_SRGB : REMIXAPI_FORMAT_BC3_UNORM;
    out.rgba8 = false;
    return true;
  case D3DFMT_DXT2:
  case D3DFMT_DXT3: // BC2 is not an API format
    out.data = decodeBC2(packed, width, height);
    break;
  default: {
    const uint32_t pitch = (rowBytes(layout, width) + 3) & ~3u;
    out.data.resize(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x)
        pixel(format, packed + size_t(y) * pitch + size_t(x) * layout.blockBytes, &out.data[(size_t(y) * width + x) * 4]);
    break;
  }
  }
  out.format = srgb ? REMIXAPI_FORMAT_R8G8B8A8_SRGB : REMIXAPI_FORMAT_R8G8B8A8_UNORM;
  out.rgba8 = true;
  return true;
}

std::vector<uint8_t> decodeBC1(const uint8_t *blocks, uint32_t width, uint32_t height) {
  return decodeBlocks(blocks, width, height, 8, [](const uint8_t *b, uint8_t p[16][4]) { colorBlock(b, true, p); });
}

std::vector<uint8_t> decodeBC2(const uint8_t *blocks, uint32_t width, uint32_t height) {
  return decodeBlocks(blocks, width, height, 16, [](const uint8_t *b, uint8_t p[16][4]) {
    colorBlock(b + 8, false, p);
    for (int i = 0; i < 16; ++i)
      p[i][3] = expand((b[i / 2] >> (4 * (i & 1))) & 15, 4);
  });
}

std::vector<uint8_t> decodeBC3(const uint8_t *blocks, uint32_t width, uint32_t height) {
  return decodeBlocks(blocks, width, height, 16, [](const uint8_t *b, uint8_t p[16][4]) {
    colorBlock(b + 8, false, p);
    uint8_t a[8] = {b[0], b[1]};
    if (a[0] > a[1]) {
      for (int i = 1; i < 7; ++i)
        a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1]) / 7);
    } else {
      for (int i = 1; i < 5; ++i)
        a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1]) / 5);
      a[6] = 0;
      a[7] = 255;
    }
    uint64_t bits = 0;
    std::memcpy(&bits, b + 2, 6);
    for (int i = 0; i < 16; ++i)
      p[i][3] = a[(bits >> (3 * i)) & 7];
  });
}

} // namespace openremix::d3d9
