#pragma once
#include "config.h"
#include "draw.h"

// What becomes of a draw, as dxvk-remix and Remix Plus decide it
// (D3D9Rtx::makeDrawCallType, isRenderingUI, processTextures).
namespace openremix::d3d9 {

enum class DrawKind {
  Forward, // rasterized by the real device only (dxvk-remix: rasterized)
  Skip,    // not traced, for a reason dxvk-remix ignores it for (also left to the real device)
  Ui,      // composited over the frame in draw order
  World,   // a path-traced instance
};

// The stage the draw's material comes from (Draw::textureStage, texture0),
// as processTextures<FixedFunction> picks it: the used, bound 2D textures
// (cube maps with rtx.allowCubemaps) up to the first disabled stage, binned
// by texture coordinate set, lightmaps (rtx.lightmapTextures) left out; the
// first of the lowest set. False when that texture has no hash: dxvk-remix
// skips such draws.
bool selectTexture(Draw &draw, const Config &config);

// `reason`, when given, says why (openremix.drawLog).
DrawKind classify(const Draw &draw, const Config &config, bool pretransformed, const char **reason = nullptr);

const char *kindName(DrawKind kind);

} // namespace openremix::d3d9
