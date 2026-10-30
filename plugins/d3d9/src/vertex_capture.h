#pragma once
#include "draw.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

// Vertex capture (dxvk-remix rtx.useVertexCapture) for a real D3D9 device:
// the draw's vertex shader runs on Direct3D's own software vertex processor
// (ProcessVertices on a private device), and its clip-space positions are
// taken back to object space through the draw's fixed-function transforms,
// as Remix Plus's DxsoCompiler::emitVertexCaptureOp takes them:
//   view = (clip x inverse(projection)) / w, world = view x inverse(view),
//   object = world x inverse(world).
// Remix-compatible games that draw with shaders keep D3DTS_VIEW and
// D3DTS_PROJECTION (and D3DTS_WORLD) up to date for this.
namespace openremix::d3d9 {

// The outputs the capture reads, beside the position.
enum : uint32_t { kOutputColor = 1, kOutputTexcoord = 2, kOutputNormal = 4 };

// A vertex shader's bytecode, kept from CreateVertexShader.
struct ShaderCode {
  std::vector<DWORD> tokens;   // as the game made it
  uint64_t hash = 0;           // XXH3 of the tokens
  uint32_t major = 0, minor = 0;
  // The shader as vs_3_0 (ProcessVertices runs vs_3_0 only): the game's for
  // vs_3_0, translated for vs_1_x and vs_2_x (toShaderModel3). Empty: not
  // capturable.
  std::vector<DWORD> model3;
  uint32_t outputs = 0;        // kOutput* the vs_3_0 shader writes
  bool vertexTextures = false; // samples textures (texldl): not captured
};

// The tokens up to and including D3DSIO_END; empty if the stream is not a
// vertex shader or does not end within `limit` tokens.
std::vector<DWORD> shaderTokens(const DWORD *code, size_t limit = 1u << 20);
// A vs_1_x or vs_2_x shader as vs_3_0: its outputs declared (oT# as o0-o7,
// oPos o8, oD0 o9, oD1 o10, oFog o11; oPts dropped), vs_1_x's a0 moves as
// mova and its relative addressing given its register, vs_2_x's sincos
// without its helper constants, every instruction's length set. Empty if the
// shader uses what has no translation. `outputs`: kOutput* it writes.
std::vector<DWORD> toShaderModel3(const std::vector<DWORD> &tokens, uint32_t &outputs);
// Parses version and the instructions the capture cares about.
ShaderCode parseShader(const DWORD *code);

struct CapturedVertex {
  float clip[4];
  float texcoord[2];
  float normal[3];
  uint32_t color; // RGBA, R in the low byte
};

// From clip space to object space: the draw's transforms inverted.
// `worldMatrices` (rtx.useWorldMatricesForShaders) false: to world space.
struct Unprojection {
  D3DMATRIX inverseProjection, viewToWorld, worldToObject;
  bool valid = false;
};
Unprojection makeUnprojection(const D3DMATRIX &projection, const D3DMATRIX &view, const D3DMATRIX &world,
                              bool worldMatrices);
void unproject(const Unprojection &u, const float clip[4], float out[3]);
// The inverse of a 4x4 matrix; false if singular.
bool invert(const D3DMATRIX &in, D3DMATRIX &out);

class VertexCapture {
public:
  ~VertexCapture();
  // Runs the draw's shader over the stream vertices [first, first + count).
  bool run(const Draw &draw, uint32_t first, uint32_t count, std::vector<CapturedVertex> &out);
  void release();

private:
  bool ensureDevice(IDirect3DDevice9 *game);
  IDirect3DVertexBuffer9 *streamBuffer(uint32_t stream, uint32_t bytes);

  IDirect3DDevice9 *m_device = nullptr;
  bool m_failed = false;
  IDirect3DVertexBuffer9 *m_streams[kStreams] = {};
  uint32_t m_streamSizes[kStreams] = {};
  IDirect3DVertexBuffer9 *m_output = nullptr;
  uint32_t m_outputVertices = 0;
  IDirect3DVertexDeclaration9 *m_outputs[8] = {}; // by kOutput* mask
  // The game's shaders and declarations, made again on the private device.
  std::unordered_map<uint64_t, IDirect3DVertexShader9 *> m_shaders;
  std::unordered_map<uint64_t, IDirect3DVertexDeclaration9 *> m_declarations;
};

} // namespace openremix::d3d9
