#include "classify.h"

#include <remix/remix_c.h>

#include <algorithm>

namespace openremix::d3d9 {
namespace {

// The arguments an operation reads: bit 0 arg 0, bit 1 arg 1, bit 2 arg 2.
uint32_t argsMask(DWORD op) {
  switch (op) {
  case D3DTOP_DISABLE: return 0b000;
  case D3DTOP_SELECTARG1:
  case D3DTOP_PREMODULATE: return 0b010;
  case D3DTOP_SELECTARG2: return 0b100;
  case D3DTOP_MULTIPLYADD:
  case D3DTOP_LERP: return 0b111;
  default: return 0b110;
  }
}

bool isTexture(DWORD arg) { return (arg & D3DTA_SELECTMASK) == D3DTA_TEXTURE; }

// Render-target formats dxvk-remix can sort into a texture format category;
// a shadow mask is drawn into another kind.
bool colorFormat(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_A8R8G8B8:
  case D3DFMT_X8R8G8B8:
  case D3DFMT_A8B8G8R8:
  case D3DFMT_X8B8G8R8:
  case D3DFMT_R5G6B5:
  case D3DFMT_A1R5G5B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A4R4G4B4:
  case D3DFMT_A2R10G10B10:
  case D3DFMT_A2B10G10R10:
  case D3DFMT_A16B16G16R16:
  case D3DFMT_A16B16G16R16F:
  case D3DFMT_A32B32G32R32F: return true;
  default: return false;
  }
}

bool anyTextureIn(const Draw &d, const std::unordered_set<uint64_t> &set) {
  if (set.empty())
    return false;
  for (const Stage &stage : d.stages)
    if (stage.hash && set.count(stage.hash))
      return true;
  return false;
}

} // namespace

bool selectTexture(Draw &d, const Config &c) {
  constexpr uint8_t kNone = 0xff;
  // Two candidates per texture coordinate set.
  uint8_t bins[8 * 2];
  std::fill(std::begin(bins), std::end(bins), kNone);
  for (uint32_t i = 0; i < kStages; ++i) {
    const Stage &stage = d.stages[i];
    if (!stage.bound)
      continue;
    const uint32_t *s = stage.states;
    // Later stages are not used past a disabled one.
    if (s[D3DTSS_COLOROP] == D3DTOP_DISABLE)
      break;
    const uint32_t args = argsMask(s[D3DTSS_COLOROP]) | argsMask(s[D3DTSS_ALPHAOP]);
    const uint32_t textures = (isTexture(s[D3DTSS_COLORARG0]) || isTexture(s[D3DTSS_ALPHAARG0]) ? 0b001 : 0) |
                              (isTexture(s[D3DTSS_COLORARG1]) || isTexture(s[D3DTSS_ALPHAARG1]) ? 0b010 : 0) |
                              (isTexture(s[D3DTSS_COLORARG2]) || isTexture(s[D3DTSS_ALPHAARG2]) ? 0b100 : 0);
    if (!(args & textures))
      continue;
    if (!stage.texture2D && !c.allowCubemaps)
      continue;
    if (stage.hash && c.lightmapTextures.count(stage.hash))
      continue;
    const uint32_t bin = (s[D3DTSS_TEXCOORDINDEX] & 7) * 2;
    const uint32_t sub = bins[bin] == kNone ? 0 : 1;
    if (bins[bin + sub] == kNone)
      bins[bin + sub] = uint8_t(i);
  }
  d.textureStage = -1;
  d.texture0 = 0;
  for (const uint8_t stage : bins) {
    if (stage == kNone)
      continue;
    d.textureStage = stage;
    d.texture0 = d.stages[stage].hash;
    return d.texture0 != 0;
  }
  return true;
}

DrawKind classify(const Draw &d, const Config &c, bool pretransformed, const char **reason) {
  auto because = [&](DrawKind kind, const char *why) {
    if (reason)
      *reason = why;
    return kind;
  };
  if (d.drawId < c.drawCallRangeFirst || d.drawId > c.drawCallRangeLast)
    return because(DrawKind::Skip, "draw call range");
  // A vertex shader's draw is captured (rtx.useVertexCapture) when its
  // bytecode is known.
  if (d.vertexShader && !d.shader)
    return because(DrawKind::Forward, "vertex shader not capturable");
  if (!d.primCount ||
      (d.type != D3DPT_TRIANGLELIST && d.type != D3DPT_TRIANGLESTRIP && d.type != D3DPT_TRIANGLEFAN))
    return because(DrawKind::Forward, "primitive type");
  if ((alphaTesting(d) && !c.enableAlphaTest) || (blending(d) && !c.enableAlphaBlend))
    return because(DrawKind::Skip, "alpha options");
  // The game counts what its occlusion queries see.
  if (d.occlusionQuery || !d.target)
    return because(DrawKind::Forward, "occlusion query or no target");
  constexpr DWORD rgb = D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE;
  if ((d.renderStates[D3DRS_COLORWRITEENABLE] & rgb) != rgb)
    return because(DrawKind::Forward, "color writes off");
  // A shadow mask: an untextured fill of a small square target of a format
  // no colour texture has.
  const uint32_t *s = d.stages[0].states;
  if ((s[D3DTSS_COLOROP] == D3DTOP_SELECTARG1 && s[D3DTSS_COLORARG1] != D3DTA_TEXTURE) ||
      (s[D3DTSS_COLOROP] == D3DTOP_SELECTARG2 && s[D3DTSS_COLORARG2] != D3DTA_TEXTURE))
    if (d.targetWidth == d.targetHeight && d.targetWidth < d.backBufferWidth / 4 && !colorFormat(d.targetFormat))
      return because(DrawKind::Skip, "shadow mask");
  // Another render target (dxvk-remix isRenderTargetPrimary compares sizes).
  if (d.targetWidth != d.backBufferWidth || d.targetHeight != d.backBufferHeight)
    return because(DrawKind::Forward, "other render target");
  // A stencil shadow volume.
  const DWORD zFail = d.renderStates[D3DRS_STENCILZFAIL];
  if (d.renderStates[D3DRS_STENCILENABLE] && d.renderStates[D3DRS_STENCILFUNC] == D3DCMP_ALWAYS &&
      (zFail == D3DSTENCILOP_INCR || zFail == D3DSTENCILOP_DECR || zFail == D3DSTENCILOP_INCRSAT ||
       zFail == D3DSTENCILOP_DECRSAT) &&
      !d.renderStates[D3DRS_ZWRITEENABLE])
    return because(DrawKind::Skip, "stencil shadow volume");
  // UI: an orthographic projection without depth writes, or a UI texture.
  if (!d.vertexShader && c.orthographicIsUI && d.projection._44 == 1.0f && !d.renderStates[D3DRS_ZWRITEENABLE])
    return because(DrawKind::Ui, "orthographic");
  if (anyTextureIn(d, c.uiTextures))
    return because(DrawKind::Ui, "UI texture");
  if (pretransformed)
    return because(c.preTransformedVerticesIsUI ? DrawKind::Ui : DrawKind::Forward, "pre-transformed");
  if (d.texture0)
    if (auto it = c.categories.find(d.texture0);
        it != c.categories.end() && (it->second & REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE))
      return because(DrawKind::Skip, "ignored texture");
  return because(DrawKind::World, "world");
}

const char *kindName(DrawKind kind) {
  switch (kind) {
  case DrawKind::Forward: return "forward";
  case DrawKind::Skip: return "skip";
  case DrawKind::Ui: return "ui";
  case DrawKind::World: return "world";
  }
  return "?";
}

} // namespace openremix::d3d9
