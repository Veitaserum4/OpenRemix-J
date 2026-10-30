#pragma once
#include <windows.h>

#include <d3d9.h>

#include <cstdint>

// One draw as the hooks gathered it: the device state the capture needs and
// CPU views of its vertex and index data. Pointers stay valid only for the
// duration of the draw call.
namespace openremix::d3d9 {

struct VertexStream {
  const uint8_t *data = nullptr; // whole buffer (shadow copy, or the UP pointer)
  uint32_t size = 0;
  uint32_t offset = 0, stride = 0;
  const void *buffer = nullptr;  // the buffer's identity (nullptr: UP data)
  uint32_t version = 0;          // bumped by every write to the buffer
};

struct IndexData {
  const uint8_t *data = nullptr; // whole buffer
  uint32_t size = 0;
  bool is32 = false;
  const void *buffer = nullptr;
  uint32_t version = 0;
};

constexpr uint32_t kStreams = 8;
constexpr uint32_t kStages = 8;

// An enabled fixed-function light (SetLight + LightEnable).
struct Light {
  DWORD index;
  D3DLIGHT9 light;
};

// A texture stage as the draw sees it.
struct Stage {
  uint64_t hash = 0;        // the texture's dxvk-remix hash (0: none, or not hashable)
  bool bound = false;       // a texture is bound
  bool texture2D = false;   // a 2D texture (cube and volume textures are not taken)
  uint32_t states[33] = {}; // D3DTSS_*
  uint32_t samplers[14] = {}; // D3DSAMP_*
};

struct Draw {
  D3DPRIMITIVETYPE type = D3DPT_TRIANGLELIST;
  bool indexed = false;
  int baseVertex = 0;       // DrawIndexedPrimitive BaseVertexIndex
  uint32_t startVertex = 0; // DrawPrimitive StartVertex
  uint32_t minIndex = 0, numVertices = 0, startIndex = 0, primCount = 0;
  VertexStream streams[kStreams];
  IndexData indices;
  const D3DVERTEXELEMENT9 *elements = nullptr; // terminated by D3DDECL_END
  uint64_t declarationHash = 0;                // the elements' hash

  uint32_t drawId = 0;       // this frame's draw count (rtx.drawCallRange, sky draw threshold)
  bool vertexShader = false; // a vertex shader is bound (not fixed function)
  bool occlusionQuery = false; // an occlusion query is open
  // Render target 0: the back buffer, or another surface of this size and format.
  bool target = true, backBuffer = true;
  uint32_t targetWidth = 0, targetHeight = 0, backBufferWidth = 0, backBufferHeight = 0;
  D3DFORMAT targetFormat = D3DFMT_UNKNOWN;

  Stage stages[kStages];
  // The stage the material comes from (selectTexture): its texture is the
  // albedo, its states the blend state's (-1: none).
  int textureStage = -1;
  uint64_t texture0 = 0;     // that stage's texture hash
  // Its D3DTSS_* and D3DSAMP_* (stage 0's when none).
  const uint32_t *stage0() const { return stages[textureStage < 0 ? 0 : textureStage].states; }
  const uint32_t *sampler0() const { return stages[textureStage < 0 ? 0 : textureStage].samplers; }
  uint32_t *stage0() { return stages[textureStage < 0 ? 0 : textureStage].states; }
  uint32_t *sampler0() { return stages[textureStage < 0 ? 0 : textureStage].samplers; }

  D3DMATRIX world, view, projection;
  D3DVIEWPORT9 viewport = {};
  uint32_t renderStates[256] = {};
  D3DMATERIAL9 material = {};
  const Light *lights = nullptr; // enabled lights (D3DRS_LIGHTING on)
  uint32_t lightCount = 0;
  // Fixed-function vertex blending: the device's world matrices
  // (D3DTS_WORLDMATRIX(n)), read on demand.
  IDirect3DDevice9 *device = nullptr;
};

// Render-state helpers.
inline bool blending(const Draw &d) { return d.renderStates[D3DRS_ALPHABLENDENABLE] != 0; }
inline bool alphaTesting(const Draw &d) { return d.renderStates[D3DRS_ALPHATESTENABLE] != 0; }
inline bool depthWriting(const Draw &d) {
  return d.renderStates[D3DRS_ZENABLE] != D3DZB_FALSE && d.renderStates[D3DRS_ZWRITEENABLE] != 0;
}

} // namespace openremix::d3d9
