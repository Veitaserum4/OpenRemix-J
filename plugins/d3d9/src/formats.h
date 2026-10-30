#pragma once
#include <windows.h>

#include <d3d9.h>
#include <remix/remix_c.h>

#include <cstdint>
#include <vector>

// D3D9 texture formats: their memory layout, and conversion of level 0 into
// a format the Remix API takes.
namespace openremix::d3d9 {

struct FormatLayout {
  uint32_t blockBytes = 0; // bytes per pixel, or per 4x4 block
  bool compressed = false;
};
// False for formats the plugin does not read (depth, YUV, float...).
bool formatLayout(D3DFORMAT format, FormatLayout &out);

// Bytes in one row of pixels (or of blocks), and the number of such rows.
uint32_t rowBytes(const FormatLayout &layout, uint32_t width);
uint32_t rowCount(const FormatLayout &layout, uint32_t height);

// Level 0 with rows at the 4-byte aligned pitch dxvk-remix keeps (the bytes
// its texture hash covers), from `pitch`-spaced rows.
std::vector<uint8_t> packLevel(D3DFORMAT format, uint32_t width, uint32_t height, const uint8_t *data,
                               uint32_t pitch);

// A packed level (packLevel) as Remix texture data: block-compressed formats
// as they are where the API takes them, everything else as RGBA8 (R in the
// lowest byte). `srgb` picks the sRGB variants (colour textures).
struct Converted {
  std::vector<uint8_t> data;
  remixapi_Format format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
  bool rgba8 = true;
};
bool convertLevel(D3DFORMAT format, uint32_t width, uint32_t height, const uint8_t *packed, bool srgb,
                  Converted &out);

// Tightly packed BC1/BC2/BC3 blocks to RGBA8.
std::vector<uint8_t> decodeBC1(const uint8_t *blocks, uint32_t width, uint32_t height);
std::vector<uint8_t> decodeBC2(const uint8_t *blocks, uint32_t width, uint32_t height);
std::vector<uint8_t> decodeBC3(const uint8_t *blocks, uint32_t width, uint32_t height);

} // namespace openremix::d3d9
