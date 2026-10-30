#include "translator.h"

#include "camera.h"
#include "hash.h"
#include "hooks.h"
#include "log.h"
#include "perf.h"
#include "renderer.h"
#include "vertex.h"
#include "vertex_capture.h"

#include <remix/remix_native_ext.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define XXH_INLINE_ALL
#include "xxhash.h"

namespace openremix::d3d9::translator {
namespace {

constexpr float kPi = 3.14159265f;
// Meshes and lights no draw used for this many frames are destroyed.
constexpr uint64_t kUnusedFrames = 300;
// dxvk-remix light conversion defaults (rtx_lights.h, rtx_light_manager.h).
constexpr float kLegacyLightEndValue = 1.0f / 255.0f, kNewLightEndValue = 0.01f;
constexpr float kSphereLightRadius = 4.0f, kDistantLightAngle = 0.0349f;
constexpr uint64_t kWhiteMaterial = 0x6f70656e72656d78ull;

bool g_active = false;
Config g_config;
uint64_t g_frame = 1;

std::unordered_set<uint64_t> g_materials;
std::unordered_map<uint64_t, uint64_t> g_meshes; // key -> last frame used
std::unordered_map<uint64_t, uint64_t> g_lights; // hash -> last frame used

// This frame, submitted at present.
struct Instance {
  InstanceDesc desc;
  std::vector<remixapi_Transform> bones;
};
std::vector<Instance> g_instances;
// The frame's cameras by CameraClass (Main, ViewModel, Sky): the first draw of
// each class sets its camera up. The last Main camera stays for frames
// without one.
struct Camera {
  bool set = false;
  D3DMATRIX view, projection;
};
Camera g_cameras[3];
Camera g_lastMain;
CameraFrame g_cameraFrame;
bool g_fogSet = false;
FogDesc g_fog = {}, g_sentFog = {};

// Geometry memoization (Remix Plus D3D9Rtx::buildGeometryHashMemoizationKey):
// a draw of the same buffers at the same versions and ranges has the same
// decoded vertices and asset hash.
struct Geometry {
  Mesh mesh;          // decoded, before the lighting colour
  uint64_t asset = 0; // geometry asset hash (0: none)
  uint64_t content = 0;
};
constexpr size_t kMemoEntries = 16384;
std::unordered_map<uint64_t, std::shared_ptr<const Geometry>> g_memo;
uint32_t g_memoHits = 0, g_memoMisses = 0;
// Vertex capture's private device (device thread).
VertexCapture g_capture;
uint32_t g_captured = 0, g_captureFailures = 0;
std::vector<UiVertex> g_uiVertices;
std::vector<uint32_t> g_uiIndices;
std::vector<UiCommand> g_uiCommands;
uint32_t g_kinds[4] = {}; // draws per DrawKind this frame
// openremix.hashLog: the material textures, replacement keys (each with its
// geometry asset hash and texture) and lights seen.
std::set<uint64_t> g_seenTextures, g_seenLights;
std::map<uint64_t, std::pair<uint64_t, uint64_t>> g_seenReplacements;

void writeHashLog() {
  FILE *file = std::fopen(g_config.hashLog.c_str(), "w");
  if (!file)
    return;
  for (uint64_t hash : g_seenTextures)
    std::fprintf(file, "texture %016llX\n", static_cast<unsigned long long>(hash));
  for (const auto &[key, parts] : g_seenReplacements)
    std::fprintf(file, "mesh %016llX asset %016llX texture %016llX\n", static_cast<unsigned long long>(key),
                 static_cast<unsigned long long>(parts.first), static_cast<unsigned long long>(parts.second));
  for (uint64_t hash : g_seenLights)
    std::fprintf(file, "light %016llX\n", static_cast<unsigned long long>(hash));
  std::fclose(file);
}

// --- Matrices (D3D row vectors: v' = v * M) ---------------------------------
D3DMATRIX multiply(const D3DMATRIX &a, const D3DMATRIX &b) {
  D3DMATRIX out;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      float s = 0;
      for (int k = 0; k < 4; ++k)
        s += a.m[r][k] * b.m[k][c];
      out.m[r][c] = s;
    }
  return out;
}

// Remix transforms are 3x4 with column vectors: the transpose of D3D's.
void transformOf(const D3DMATRIX &m, float (&t)[3][4]) {
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c)
      t[r][c] = m.m[c][r];
}

// --- D3D9 state in the API's (Vulkan) encodings -----------------------------
uint32_t blendFactor(DWORD d3d) {
  switch (d3d) {
  case D3DBLEND_ZERO: return 0;
  case D3DBLEND_ONE: return 1;
  case D3DBLEND_SRCCOLOR: return 2;
  case D3DBLEND_INVSRCCOLOR: return 3;
  case D3DBLEND_DESTCOLOR: return 4;
  case D3DBLEND_INVDESTCOLOR: return 5;
  case D3DBLEND_SRCALPHA:
  case D3DBLEND_BOTHSRCALPHA: return 6;
  case D3DBLEND_INVSRCALPHA:
  case D3DBLEND_BOTHINVSRCALPHA: return 7;
  case D3DBLEND_DESTALPHA: return 8;
  case D3DBLEND_INVDESTALPHA: return 9;
  case D3DBLEND_BLENDFACTOR: return 10;
  case D3DBLEND_INVBLENDFACTOR: return 11;
  case D3DBLEND_SRCALPHASAT: return 14;
  default: return 1;
  }
}

uint32_t blendOp(DWORD d3d) { return d3d >= D3DBLENDOP_ADD && d3d <= D3DBLENDOP_MAX ? d3d - 1 : 0; }
uint32_t compareOp(DWORD d3d) { return d3d >= D3DCMP_NEVER && d3d <= D3DCMP_ALWAYS ? d3d - 1 : 7; }

// D3DTOP to the runtime's texture operations (0 disabled, 1 select arg 1,
// 2 select arg 2, 3/4/5 modulate 1x/2x/4x, 6 add).
uint32_t textureOp(DWORD d3d) {
  switch (d3d) {
  case D3DTOP_SELECTARG1: return 1;
  case D3DTOP_SELECTARG2: return 2;
  case D3DTOP_MODULATE: return 3;
  case D3DTOP_MODULATE2X: return 4;
  case D3DTOP_MODULATE4X: return 5;
  case D3DTOP_ADD: return 6;
  default: return 3;
  }
}
// D3DTA to the runtime's arguments (0 one, 1 texture, 2 vertex colour, 3 texture factor).
uint32_t textureArg(DWORD d3d) {
  switch (d3d & D3DTA_SELECTMASK) {
  case D3DTA_TEXTURE: return 1;
  case D3DTA_DIFFUSE:
  case D3DTA_CURRENT: return 2;
  case D3DTA_TFACTOR: return 3;
  default: return 0;
  }
}

uint8_t wrapMode(DWORD address) {
  switch (address) {
  case D3DTADDRESS_CLAMP: return 0;
  case D3DTADDRESS_MIRROR:
  case D3DTADDRESS_MIRRORONCE: return 2;
  case D3DTADDRESS_BORDER: return 3;
  default: return 1;
  }
}

BlendState blendState(const Draw &d) {
  BlendState b = {};
  b.alphaTestEnabled = alphaTesting(d);
  b.alphaTestReference = d.renderStates[D3DRS_ALPHAREF] & 255;
  b.alphaTestCompareOp = compareOp(d.renderStates[D3DRS_ALPHAFUNC]);
  b.alphaBlendEnabled = blending(d);
  b.srcColorBlendFactor = blendFactor(d.renderStates[D3DRS_SRCBLEND]);
  b.dstColorBlendFactor = blendFactor(d.renderStates[D3DRS_DESTBLEND]);
  b.colorBlendOp = blendOp(d.renderStates[D3DRS_BLENDOP]);
  if (d.renderStates[D3DRS_SEPARATEALPHABLENDENABLE]) {
    b.srcAlphaBlendFactor = blendFactor(d.renderStates[D3DRS_SRCBLENDALPHA]);
    b.dstAlphaBlendFactor = blendFactor(d.renderStates[D3DRS_DESTBLENDALPHA]);
    b.alphaBlendOp = blendOp(d.renderStates[D3DRS_BLENDOPALPHA]);
  } else {
    b.srcAlphaBlendFactor = b.srcColorBlendFactor;
    b.dstAlphaBlendFactor = b.dstColorBlendFactor;
    b.alphaBlendOp = b.colorBlendOp;
  }
  b.writeMask = d.renderStates[D3DRS_COLORWRITEENABLE] & 15;
  if (d.stage0()[D3DTSS_COLOROP] == D3DTOP_DISABLE) {
    b.textureColorOperation = 1;
    b.textureColorArg1Source = 2;
  } else {
    b.textureColorOperation = textureOp(d.stage0()[D3DTSS_COLOROP]);
    b.textureColorArg1Source = textureArg(d.stage0()[D3DTSS_COLORARG1]);
    b.textureColorArg2Source = textureArg(d.stage0()[D3DTSS_COLORARG2]);
  }
  if (d.stage0()[D3DTSS_ALPHAOP] == D3DTOP_DISABLE) {
    b.textureAlphaOperation = 1;
    b.textureAlphaArg1Source = 2;
  } else {
    b.textureAlphaOperation = textureOp(d.stage0()[D3DTSS_ALPHAOP]);
    b.textureAlphaArg1Source = textureArg(d.stage0()[D3DTSS_ALPHAARG1]);
    b.textureAlphaArg2Source = textureArg(d.stage0()[D3DTSS_ALPHAARG2]);
  }
  b.tFactor = d.renderStates[D3DRS_TEXTUREFACTOR];
  return b;
}

uint32_t packColor(const D3DCOLORVALUE &c) {
  auto byte = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
  return byte(c.r) | byte(c.g) << 8 | byte(c.b) << 16 | byte(c.a) << 24;
}

// The draw's material: its texture's hash (as dxvk-remix keys materials), or
// the white material for untextured draws. Made on first use.
uint64_t material(const Draw &d) {
  const uint64_t hash = d.texture0 ? d.texture0 : kWhiteMaterial;
  if (g_materials.insert(hash).second) {
    MaterialDesc desc = {};
    desc.hash = hash;
    desc.texture = d.texture0;
    desc.filter = d.sampler0()[D3DSAMP_MAGFILTER] == D3DTEXF_POINT ? 0 : 1;
    desc.wrapU = wrapMode(d.sampler0()[D3DSAMP_ADDRESSU]);
    desc.wrapV = wrapMode(d.sampler0()[D3DSAMP_ADDRESSV]);
    renderer().createMaterial(desc);
  }
  return hash;
}

// --- Lights -----------------------------------------------------------------
// dxvk-remix's stable hash of a D3D9 light (LightData::createFromPointSpot,
// createFromDirectional): what a mod's light_<hash> overrides name. Radiance
// is left out; a point or spot light hashes as a sphere light of the legacy
// radius 4, a directional light as a rect light (a dxvk-remix bug kept for
// compatibility) with the legacy half angle.
uint64_t lightHash(const D3DLIGHT9 &l) {
  constexpr uint64_t kSphere = 0, kRect = 1;
  if (l.Type == D3DLIGHT_DIRECTIONAL) {
    const float halfAngle = 0.0349f / 2.0f;
    uint64_t h = XXH64(&l.Direction, sizeof(l.Direction), kRect);
    return XXH64(&halfAngle, sizeof(halfAngle), h);
  }
  uint64_t shaping = 0;
  if (l.Type == D3DLIGHT_SPOT) {
    const float coneAngle = l.Phi / 2.0f;
    const float cosConeAngle = std::cos(coneAngle);
    const float softness = std::cos(l.Theta / 2.0f) - std::cos(coneAngle);
    shaping = XXH64(&l.Direction, sizeof(l.Direction), shaping);
    shaping = XXH64(&cosConeAngle, sizeof(cosConeAngle), shaping);
    shaping = XXH64(&softness, sizeof(softness), shaping);
    shaping = XXH64(&l.Falloff, sizeof(l.Falloff), shaping);
  }
  const float legacyRadius = 4.0f;
  uint64_t h = XXH64(&l.Position, sizeof(l.Position), kSphere);
  h = XXH64(&legacyRadius, sizeof(legacyRadius), h);
  return XXH64(&h, sizeof(h), shaping);
}

void addLight(const D3DLIGHT9 &l) {
  const uint64_t hash = lightHash(l);
  if (!g_config.hashLog.empty())
    g_seenLights.insert(hash);
  if (g_config.ignoreLights.count(hash))
    return;
  auto [it, created] = g_lights.try_emplace(hash, g_frame);
  it->second = g_frame;
  if (!created)
    return;
  const float brightness = std::max({l.Diffuse.r, l.Diffuse.g, l.Diffuse.b});
  if (brightness <= 0)
    return;
  const float color[3] = {l.Diffuse.r / brightness, l.Diffuse.g / brightness, l.Diffuse.b / brightness};
  auto normalized = [](const D3DVECTOR &v, float (&out)[3]) {
    const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    out[0] = length > 0 ? v.x / length : 0, out[1] = length > 0 ? v.y / length : 0,
    out[2] = length > 0 ? v.z / length : 1;
  };
  LightDesc desc = {};
  float radiance;
  if (l.Type == D3DLIGHT_DIRECTIONAL) {
    // A fixed intensity of 1 W/sr spread over the light's disc.
    const float half = kDistantLightAngle / 2;
    radiance = 1.0f / (kPi * std::sin(half) * std::sin(half));
    desc.distant = 1;
    normalized(l.Direction, desc.direction);
    desc.angularDiameter = kDistantLightAngle * 180 / kPi;
  } else {
    // dxvk-remix LightUtils::calculateIntensity: the distance where the D3D9
    // attenuation reaches 1/255 of the brightest channel.
    const float a = l.Attenuation2, b = l.Attenuation1, c = l.Attenuation0;
    float end = l.Range;
    if (c > 0 && brightness / c < kLegacyLightEndValue) {
      end = 0;
    } else if (a < 1e-6f) {
      if (b > 1e-6f)
        end = (brightness / kLegacyLightEndValue - c) / b;
    } else {
      const float cc = c - brightness / kLegacyLightEndValue, discriminant = b * b - 4 * a * cc;
      if (discriminant >= 0) {
        const float root = std::sqrt(discriminant);
        const float r1 = (-b + root) / (2 * a), r2 = (-b - root) / (2 * a);
        end = r2 > 0 ? r2 : r1 > 0 ? r1 : end;
      }
    }
    radiance = kNewLightEndValue / (kPi * kSphereLightRadius * kSphereLightRadius) * end * end;
    desc.position[0] = l.Position.x, desc.position[1] = l.Position.y, desc.position[2] = l.Position.z;
    desc.radius = kSphereLightRadius;
    if (l.Type == D3DLIGHT_SPOT) {
      desc.shaped = 1;
      normalized(l.Direction, desc.direction);
      desc.coneAngle = l.Phi / 2 * 180 / kPi;
      desc.coneSoftness = std::cos(l.Theta / 2) - std::cos(l.Phi / 2);
      desc.focus = l.Falloff;
    }
  }
  for (int i = 0; i < 3; ++i)
    desc.radiance[i] = color[i] * radiance;
  renderer().createLight(hash, desc);
}

// --- World draws ------------------------------------------------------------
// The draw's memoization key, or 0 when it reads data no buffer version
// covers (UP draws). Without `shaderState`: the same draw whatever its
// shader's constants and transforms (a captured draw's identity).
uint64_t memoKey(const Draw &d, bool shaderState = true) {
  uint32_t used = 0;
  for (const D3DVERTEXELEMENT9 *e = d.elements; e->Stream != 0xff; ++e)
    if (e->Stream < kStreams)
      used |= 1u << e->Stream;
  uint64_t key = XXH3_64bits_withSeed(&g_config.assetHashRule, 4, d.declarationHash);
  for (uint32_t i = 0; i < kStreams; ++i) {
    if (!(used & (1u << i)))
      continue;
    const VertexStream &s = d.streams[i];
    if (!s.buffer)
      return 0;
    const uint64_t fields[4] = {uint64_t(uintptr_t(s.buffer)), s.version, s.offset, s.stride};
    key = XXH3_64bits_withSeed(fields, sizeof(fields), key);
  }
  if (d.indexed) {
    if (!d.indices.buffer)
      return 0;
    const uint64_t fields[4] = {uint64_t(uintptr_t(d.indices.buffer)), d.indices.version, d.indices.is32,
                                uint64_t(uint32_t(d.baseVertex))};
    key = XXH3_64bits_withSeed(fields, sizeof(fields), key);
  }
  const uint32_t draw[6] = {uint32_t(d.type), d.startVertex, d.startIndex, d.primCount,
                            d.stage0()[D3DTSS_TEXCOORDINDEX] & 7, d.indexed};
  key = XXH3_64bits_withSeed(draw, sizeof(draw), key);
  // A captured draw's vertices are its shader's, from its constants, taken
  // back through its transforms.
  if (d.shader) {
    key = XXH3_64bits_withSeed(&d.shader->hash, sizeof(d.shader->hash), key);
    if (shaderState) {
      key = XXH3_64bits_withSeed(&d.constantsHash, sizeof(d.constantsHash), key);
      const D3DMATRIX transforms[3] = {d.world, d.view, d.projection};
      key = XXH3_64bits_withSeed(transforms, sizeof(transforms), key);
      key = XXH3_64bits_withSeed(&d.pixelShader, sizeof(d.pixelShader), key);
    }
  }
  return key | 1;
}

// Captured draws' latest meshes, by the draw's identity: an animated shader
// makes a mesh each frame, and the one it replaces goes at once instead of
// after kUnusedFrames.
struct CapturedMesh {
  uint64_t mesh = 0, frame = 0;
};
std::unordered_map<uint64_t, CapturedMesh> g_capturedMeshes;

void replaceCapturedMesh(const Draw &d, uint64_t mesh) {
  const uint64_t identity = memoKey(d, false);
  if (!identity)
    return;
  CapturedMesh &owner = g_capturedMeshes[identity];
  if (owner.mesh && owner.mesh != mesh)
    if (auto it = g_meshes.find(owner.mesh); it != g_meshes.end() && it->second < g_frame) {
      renderer().destroyMesh(it->first);
      g_meshes.erase(it);
    }
  owner = {mesh, g_frame};
}

// A vertex shader's draw (rtx.useVertexCapture): its decoded input vertices
// take the shader's output positions, back in object space (Remix Plus
// emitVertexCaptureOp), and with rtx.useVertexCapturedTexcoords (or when the
// input has none) its texture coordinates; its colours without a pixel
// shader. The asset hash stays the input positions', as in dxvk-remix.
bool captureVertices(const Draw &d, const Triangles &t, Mesh &mesh) {
  perf::Timer timer(perf::g_counters.vertexCapture);
  const Unprojection unprojection =
      makeUnprojection(d.projection, d.view, d.world, g_config.useWorldMatricesForShaders);
  std::vector<CapturedVertex> captured;
  bool ran;
  {
    hooks::InternalCalls internal;
    ran = unprojection.valid && g_capture.run(d, t.low, t.high - t.low + 1, captured);
  }
  if (!ran) {
    ++g_captureFailures;
    return false;
  }
  ++g_captured;
  const BYTE set = BYTE(d.stage0()[D3DTSS_TEXCOORDINDEX] & 7);
  const bool texcoords = (d.shader->outputs & kOutputTexcoord) &&
                         (g_config.useVertexCapturedTexcoords || !findElement(d.elements, D3DDECLUSAGE_TEXCOORD, set));
  const bool colors = (d.shader->outputs & kOutputColor) && !d.pixelShader;
  for (size_t i = 0; i < mesh.vertices.size(); ++i) {
    Vertex &v = mesh.vertices[i];
    const CapturedVertex &c = captured[i];
    unproject(unprojection, c.clip, v.position);
    if (texcoords)
      v.texcoord[0] = c.texcoord[0], v.texcoord[1] = c.texcoord[1];
    if (!g_config.useVertexCapturedNormals)
      v.normal[0] = v.normal[1] = v.normal[2] = 0;
    if (colors)
      v.color = c.color;
  }
  return true;
}

// The draw's decoded geometry and asset hash, from the memo when it can be.
// nullptr when the draw cannot be read (or dxvk-remix would drop it).
std::shared_ptr<const Geometry> geometryFor(const Draw &d) {
  const uint64_t key = memoKey(d);
  if (key)
    if (auto it = g_memo.find(key); it != g_memo.end()) {
      ++g_memoHits;
      return it->second;
    }
  ++g_memoMisses;
  perf::Timer timer(perf::g_counters.geometry);
  auto g = std::make_shared<Geometry>();
  Triangles triangles;
  bool degenerate = false;
  if (!triangulate(d, triangles) || !decode(d, triangles, g->mesh))
    return nullptr;
  g->asset = geometryHash(d, g_config.assetHashRule, &degenerate);
  if (degenerate)
    return nullptr;
  if (d.shader && !captureVertices(d, triangles, g->mesh))
    return nullptr;
  const Mesh &m = g->mesh;
  uint64_t content = XXH3_64bits(m.vertices.data(), m.vertices.size() * sizeof(Vertex));
  content = XXH3_64bits_withSeed(m.indices.data(), m.indices.size() * 4, content);
  content = XXH3_64bits_withSeed(m.weights.data(), m.weights.size() * 4, content);
  g->content = XXH3_64bits_withSeed(m.blendIndices.data(), m.blendIndices.size() * 4, content);
  if (key) {
    if (g_memo.size() >= kMemoEntries)
      g_memo.clear();
    g_memo.emplace(key, g);
  }
  return g;
}

// Fixed-function vertex blending (dxvk-remix D3D9Rtx::processSkinning): per
// vertex D3DRS_VERTEXBLEND weights (the last one 1 less the others) of the
// world matrices D3DTS_WORLDMATRIX(n), n the vertex's blend indices or its
// weights' order. A draw that uses one matrix only takes it as its world.
// The matrices place the vertices in the draw's object space: place() takes
// them to the world (dxvk-remix DrawCallState::finalizeSkinningData).
struct Skin {
  uint32_t bonesPerVertex = 0;
  std::vector<float> weights;
  std::vector<uint32_t> indices;
  std::vector<D3DMATRIX> matrices; // D3DTS_WORLDMATRIX(0..)
  std::vector<remixapi_Transform> bones; // in the world (place)
  int single = -1; // the one matrix used
  D3DMATRIX singleMatrix;
};

bool skinning(const Draw &d, const Mesh &m, Skin &out) {
  const DWORD blend = d.renderStates[D3DRS_VERTEXBLEND];
  if (d.vertexShader || blend == D3DVBF_DISABLE || blend == D3DVBF_TWEENING || !d.device)
    return false;
  const uint32_t given = blend == D3DVBF_0WEIGHTS ? 0 : uint32_t(blend);
  if (given > 3 || given > m.weightCount)
    return false;
  const bool indexed = d.renderStates[D3DRS_INDEXEDVERTEXBLENDENABLE] && !m.blendIndices.empty();
  const uint32_t per = given + 1, count = uint32_t(m.vertices.size());
  out.bonesPerVertex = per;
  out.weights.resize(size_t(count) * per);
  out.indices.resize(size_t(count) * per);
  uint32_t low = 255, high = 0;
  for (uint32_t v = 0; v < count; ++v) {
    float sum = 0;
    for (uint32_t k = 0; k < given; ++k) {
      const float w = m.weights[size_t(v) * m.weightCount + k];
      out.weights[size_t(v) * per + k] = w;
      sum += w;
    }
    out.weights[size_t(v) * per + given] = 1 - sum;
    for (uint32_t k = 0; k < per; ++k) {
      const uint32_t bone = indexed ? std::min(m.blendIndices[size_t(v) * 4 + k], 255u) : k;
      out.indices[size_t(v) * per + k] = bone;
      low = std::min(low, bone), high = std::max(high, bone);
    }
  }
  if (low == high) {
    out.single = int(low);
    d.device->GetTransform(D3DTS_WORLDMATRIX(low), &out.singleMatrix);
    return true;
  }
  out.matrices.resize(high + 1);
  for (uint32_t i = 0; i <= high; ++i)
    d.device->GetTransform(D3DTS_WORLDMATRIX(i), &out.matrices[i]);
  return true;
}

// A skinned draw's object space in the world: its view matrix (which some
// games give the model's placement, Source's fixed-function path among
// them), out through the camera's (dxvk-remix: objectToWorld =
// viewToWorld x objectToView). Identity where the draw's view is the
// camera's.
void place(Skin &skin, const D3DMATRIX &drawView, const D3DMATRIX *cameraView) {
  D3DMATRIX placement = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
  D3DMATRIX inverse;
  if (cameraView && invert(*cameraView, inverse))
    placement = multiply(drawView, inverse);
  if (skin.single >= 0) {
    skin.singleMatrix = multiply(skin.singleMatrix, placement);
    return;
  }
  skin.bones.resize(skin.matrices.size());
  for (size_t i = 0; i < skin.matrices.size(); ++i) {
    float t[3][4];
    transformOf(multiply(skin.matrices[i], placement), t);
    std::memcpy(skin.bones[i].matrix, t, sizeof(t));
  }
}

float floatState(DWORD bits) {
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

// The frame's fog: the first Main-camera draw's (D3DRS_FOG*, table mode
// before vertex mode, as dxvk-remix setFogState reads it).
void noteFog(const Draw &d) {
  if (g_fogSet)
    return;
  g_fogSet = true;
  g_fog = {};
  if (!d.renderStates[D3DRS_FOGENABLE])
    return;
  const DWORD table = d.renderStates[D3DRS_FOGTABLEMODE];
  g_fog.mode = table != D3DFOG_NONE ? table : d.renderStates[D3DRS_FOGVERTEXMODE];
  g_fog.color = d.renderStates[D3DRS_FOGCOLOR];
  const float start = floatState(d.renderStates[D3DRS_FOGSTART]), end = floatState(d.renderStates[D3DRS_FOGEND]);
  g_fog.scale = end != start ? 1 / (end - start) : 0;
  g_fog.end = end;
  g_fog.density = floatState(d.renderStates[D3DRS_FOGDENSITY]);
}

// Why the translator left the last draw it could not take (drawLog).
const char *g_leftBecause = "";
FILE *g_drawLog = nullptr; // this frame's draws, described (openremix.drawLog)

bool worldDraw(const Draw &d) {
  const std::shared_ptr<const Geometry> geometry = geometryFor(d);
  if (!geometry) {
    g_leftBecause = d.shader ? "geometry or vertex capture failed" : "geometry not readable";
    return false;
  }
  uint32_t categories = 0;
  if (d.texture0)
    if (auto it = g_config.categories.find(d.texture0); it != g_config.categories.end())
      categories = it->second;
  const bool sky = skyDraw(d, g_config, categories);
  Skin skin;
  const bool skinned = skinning(d, geometry->mesh, skin);
  bool updatesCamera;
  const CameraClass camera = classifyCamera(d, g_config, sky, skinned, g_cameraFrame, updatesCamera);
  if (skinned) {
    // The camera the skinned matrices are decomposed with: the draw's class's
    // this frame, else the main camera (dxvk-remix: the last set up).
    const Camera *view = camera != CameraClass::Unknown && g_cameras[size_t(camera)].set
                             ? &g_cameras[size_t(camera)]
                         : g_cameras[size_t(CameraClass::Main)].set ? &g_cameras[size_t(CameraClass::Main)]
                         : g_lastMain.set                             ? &g_lastMain
                                                                      : nullptr;
    place(skin, d.view, view ? &view->view : nullptr);
  }
  if (g_drawLog && skinned && !geometry->mesh.vertices.empty()) {
    // Where the first vertex lands (drawLog): its bones, blended.
    const Mesh &m = geometry->mesh;
    const float *p = m.vertices[0].position;
    float world[3] = {};
    if (skin.single >= 0) {
      for (int c = 0; c < 3; ++c)
        world[c] = p[0] * skin.singleMatrix.m[0][c] + p[1] * skin.singleMatrix.m[1][c] +
                   p[2] * skin.singleMatrix.m[2][c] + skin.singleMatrix.m[3][c];
    } else {
      for (uint32_t k = 0; k < skin.bonesPerVertex; ++k) {
        const float w = skin.weights[k];
        const auto &t = skin.bones[skin.indices[k]].matrix;
        for (int r = 0; r < 3; ++r)
          world[r] += w * (t[r][0] * p[0] + t[r][1] * p[1] + t[r][2] * p[2] + t[r][3]);
      }
    }
    char line[256];
    std::snprintf(line, sizeof(line),
                  "  skinned: %u per vertex, %zu bones, single %d, first index %u, vertex (%.1f %.1f %.1f) -> "
                  "(%.1f %.1f %.1f), camera %d\n",
                  skin.bonesPerVertex, skin.bones.size(), skin.single, skin.indices.empty() ? 0u : skin.indices[0],
                  p[0], p[1], p[2], world[0], world[1], world[2], int(camera));
    std::fputs(line, g_drawLog);
  }
  if (updatesCamera && camera != CameraClass::Unknown) {
    Camera &c = g_cameras[size_t(camera)];
    if (!c.set) {
      c = {true, d.view, d.projection};
      if (camera == CameraClass::Main)
        noteFog(d);
    }
  }

  // The vertex colour fixed-function lighting starts from: the material's
  // diffuse, unless the vertices carry it (D3DRS_DIFFUSEMATERIALSOURCE); the
  // path tracer lights it.
  const Mesh &mesh = geometry->mesh;
  bool recolor = false;
  uint32_t color = 0;
  if (!d.vertexShader && d.renderStates[D3DRS_LIGHTING] &&
      !(d.renderStates[D3DRS_COLORVERTEX] && mesh.colors &&
        d.renderStates[D3DRS_DIFFUSEMATERIALSOURCE] == D3DMCS_COLOR1)) {
    recolor = true;
    color = packColor(d.material.Diffuse);
  }
  const uint64_t materialHash = material(d);
  const bool skinMesh = skinned && skin.single < 0;
  const uint64_t keyFields[4] = {geometry->content, recolor ? uint64_t(color) | (1ull << 32) : 0, materialHash,
                                 skinMesh ? uint64_t(skin.bonesPerVertex) : 0};
  const uint64_t key = XXH3_64bits(keyFields, sizeof(keyFields));
  auto [it, created] = g_meshes.try_emplace(key, g_frame);
  it->second = g_frame;
  if (d.shader)
    replaceCapturedMesh(d, key);
  if (created) {
    std::vector<Vertex> vertices = mesh.vertices;
    if (recolor)
      for (Vertex &v : vertices)
        v.color = color;
    Skinning blend;
    if (skinMesh) {
      blend.bonesPerVertex = skin.bonesPerVertex;
      blend.weights = skin.weights.data();
      blend.indices = skin.indices.data();
    }
    renderer().createMesh(key, vertices.data(), uint32_t(vertices.size()), mesh.indices.data(),
                          uint32_t(mesh.indices.size()), materialHash, blend);
    ++perf::g_counters.meshes;
    perf::g_counters.meshBytes += vertices.size() * sizeof(Vertex) + mesh.indices.size() * 4;
  }

  Instance instance = {};
  InstanceDesc &desc = instance.desc;
  desc.mesh = key;
  // Skinned vertices go to the world through their bones.
  static const D3DMATRIX identity = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
  // A captured draw's vertices are in object space, or in world space without
  // rtx.useWorldMatricesForShaders.
  const bool worldSpace = d.shader && !g_config.useWorldMatricesForShaders;
  transformOf(skin.single >= 0 ? skin.singleMatrix : skinMesh || worldSpace ? identity : d.world, desc.transform);
  if (skinMesh) {
    instance.bones = std::move(skin.bones);
    desc.boneCount = uint32_t(instance.bones.size());
  }
  desc.categories = categories;
  if (camera == CameraClass::Sky)
    desc.categories |= REMIXAPI_INSTANCE_CATEGORY_BIT_SKY;
  else if (camera == CameraClass::ViewModel)
    desc.categories |= REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL;
  desc.blend = blendState(d);
  // The USD mesh replacement key (dxvk-remix DrawCallState::getHash).
  desc.replacement = geometry->asset ? geometry->asset ^ d.texture0 : 0;
  if (!g_config.hashLog.empty()) {
    if (d.texture0)
      g_seenTextures.insert(d.texture0);
    if (desc.replacement)
      g_seenReplacements.try_emplace(desc.replacement, geometry->asset, d.texture0);
  }
  // Double-sided whatever D3DRS_CULLMODE says: the raster backend takes
  // clockwise faces as front (D3D's screen-space rule), the path tracer
  // decides in object space, where a left-handed D3D world's are reversed.
  desc.doubleSided = 1;
  // Additive draws (glows, flares) have no surface to trace.
  desc.rasterOnly = desc.blend.alphaBlendEnabled && desc.blend.dstColorBlendFactor == 1;
  g_instances.push_back(std::move(instance));
  return true;
}

// --- UI draws ---------------------------------------------------------------
bool uiDraw(const Draw &d) {
  Triangles triangles;
  Mesh mesh;
  if (!triangulate(d, triangles) || !decode(d, triangles, mesh))
    return false;
  const D3DMATRIX wvp = multiply(multiply(d.world, d.view), d.projection);
  const float vx = float(d.viewport.X), vy = float(d.viewport.Y), vw = float(d.viewport.Width),
              vh = float(d.viewport.Height);
  // What stage 0 takes from the vertex colour and from the texture.
  const DWORD colorOp = d.stage0()[D3DTSS_COLOROP], alphaOp = d.stage0()[D3DTSS_ALPHAOP];
  const bool textureOnlyColor =
      colorOp == D3DTOP_SELECTARG1 && (d.stage0()[D3DTSS_COLORARG1] & D3DTA_SELECTMASK) == D3DTA_TEXTURE;
  const bool textureOnlyAlpha =
      alphaOp == D3DTOP_SELECTARG1 && (d.stage0()[D3DTSS_ALPHAARG1] & D3DTA_SELECTMASK) == D3DTA_TEXTURE;
  const bool textured = d.texture0 && colorOp != D3DTOP_DISABLE;

  const uint32_t base = uint32_t(g_uiVertices.size());
  for (size_t i = 0; i < mesh.vertices.size(); ++i) {
    const auto &v = mesh.vertices[i];
    UiVertex out = {};
    if (mesh.pretransformed) {
      // D3D9 pixel centres are at integers, Vulkan's at half integers.
      out.x = v.position[0] + 0.5f;
      out.y = v.position[1] + 0.5f;
      out.z = std::clamp(v.position[2], 0.0f, 1.0f);
    } else {
      float clip[4];
      for (int c = 0; c < 4; ++c)
        clip[c] = v.position[0] * wvp.m[0][c] + v.position[1] * wvp.m[1][c] + v.position[2] * wvp.m[2][c] +
                  wvp.m[3][c];
      const float w = std::fabs(clip[3]) > 1e-6f ? clip[3] : 1e-6f;
      out.x = vx + (clip[0] / w * 0.5f + 0.5f) * vw;
      out.y = vy + (0.5f - clip[1] / w * 0.5f) * vh;
      out.z = std::clamp(d.viewport.MinZ + clip[2] / w * (d.viewport.MaxZ - d.viewport.MinZ), 0.0f, 1.0f);
    }
    out.u = v.texcoord[0];
    out.v = v.texcoord[1];
    uint32_t color = v.color;
    if (textured && textureOnlyColor)
      color |= 0x00ffffffu;
    if (textured && textureOnlyAlpha)
      color |= 0xff000000u;
    out.color = color;
    g_uiVertices.push_back(out);
  }
  UiCommand command = {};
  command.texture = textured ? d.texture0 : 0;
  command.indexCount = uint32_t(mesh.indices.size());
  command.indexOffset = uint32_t(g_uiIndices.size());
  for (uint32_t index : mesh.indices)
    g_uiIndices.push_back(base + index);
  uint32_t src = 1, dst = 0; // opaque
  if (blending(d)) {
    src = blendFactor(d.renderStates[D3DRS_SRCBLEND]);
    dst = blendFactor(d.renderStates[D3DRS_DESTBLEND]);
  } else if (alphaTesting(d)) {
    src = 6, dst = 7; // cut-outs: their transparent texels stay out
  }
  command.flags = REMIXAPI_UI_NATIVE_CUSTOM_BLEND | src << REMIXAPI_UI_NATIVE_SRC_BLEND_SHIFT |
                  dst << REMIXAPI_UI_NATIVE_DST_BLEND_SHIFT;
  if (d.sampler0()[D3DSAMP_MAGFILTER] != D3DTEXF_POINT)
    command.flags |= REMIXAPI_UI_NATIVE_LINEAR;
  if (d.sampler0()[D3DSAMP_ADDRESSU] == D3DTADDRESS_WRAP)
    command.flags |= REMIXAPI_UI_NATIVE_REPEAT_U;
  if (d.sampler0()[D3DSAMP_ADDRESSV] == D3DTADDRESS_WRAP)
    command.flags |= REMIXAPI_UI_NATIVE_REPEAT_V;
  if (!mesh.pretransformed && d.renderStates[D3DRS_ZENABLE] != D3DZB_FALSE)
    command.flags |= REMIXAPI_UI_DRAW_FLAG_DEPTH_TEST;
  g_uiCommands.push_back(command);
  return true;
}

// Destroys the meshes and lights no frame used for kUnusedFrames.
void collect() {
  for (auto it = g_capturedMeshes.begin(); it != g_capturedMeshes.end();)
    if (g_frame - it->second.frame > kUnusedFrames)
      it = g_capturedMeshes.erase(it);
    else
      ++it;
  for (auto it = g_meshes.begin(); it != g_meshes.end();)
    if (g_frame - it->second > kUnusedFrames) {
      renderer().destroyMesh(it->first);
      it = g_meshes.erase(it);
    } else {
      ++it;
    }
  for (auto it = g_lights.begin(); it != g_lights.end();)
    if (g_frame - it->second > kUnusedFrames) {
      renderer().destroyLight(it->first);
      it = g_lights.erase(it);
    } else {
      ++it;
    }
}

} // namespace

bool start(HWND window, const Config &config, const std::filesystem::path &pluginDirectory,
           const std::filesystem::path &gameDirectory) {
  if (g_active)
    return true;
  g_config = config;
  g_active = renderer().start(window, config, pluginDirectory, gameDirectory);
  return g_active;
}

bool active() { return g_active && renderer().alive(); }

void shutdown() {
  {
    hooks::InternalCalls internal;
    g_capture.release();
  }
  if (!g_active)
    return;
  g_active = false;
  renderer().shutdown();
}

void textureFilled(uint64_t hash, D3DFORMAT format, uint32_t width, uint32_t height,
                   const std::vector<uint8_t> &packed) {
  ++perf::g_counters.textures;
  perf::g_counters.textureBytes += packed.size();
  renderer().storeTexture(hash, uint32_t(format), width, height, packed.data(), packed.size());
}

void lights(const std::vector<D3DLIGHT9> &enabled) {
  if (!g_active)
    return;
  for (const D3DLIGHT9 &light : enabled)
    addLight(light);
}

bool loggingDraws() { return g_drawLog != nullptr; }
void logDraw(const char *line) {
  if (g_drawLog)
    std::fputs(line, g_drawLog);
}

bool draw(const Draw &d, DrawKind kind, const char **why) {
  if (!g_active)
    return false;
  g_leftBecause = "";
  perf::Timer timer(perf::g_counters.translate);
  const bool taken = (kind == DrawKind::World && worldDraw(d)) || (kind == DrawKind::Ui && uiDraw(d));
  // A draw the translator could not read stays the real device's.
  if (why && !taken && (kind == DrawKind::World || kind == DrawKind::Ui))
    *why = *g_leftBecause ? g_leftBecause : "not decodable";
  const size_t counted = size_t(taken || kind == DrawKind::Skip ? kind : DrawKind::Forward);
  ++g_kinds[counted];
  ++perf::g_counters.kinds[counted];
  return taken;
}

// Every few seconds: the frame rate and where the frames' time went.
void reportPerformance() {
  static int64_t since = perf::now();
  static uint64_t sinceFrame = g_frame;
  const int64_t now = perf::now();
  const double elapsed = perf::milliseconds(now - since);
  if (elapsed < 2000)
    return;
  perf::Counters &c = perf::g_counters;
  const double frames = double(g_frame - sinceFrame);
  auto per = [&](int64_t ticks) { return perf::milliseconds(ticks) / frames; };
  log("performance, frames %llu-%llu: %.1f fps, %.1f ms a frame; per frame: draws %.0f (%.0f forwarded, %.0f skipped, %.0f UI, %.0f world) taking %.2f ms "
      "(translator %.2f, decoding %.2f, vertex capture %.2f), texture reads %.2f ms, present %.2f ms, "
      "bridge sends %.2f ms; %.1f meshes (%.1f KB), %.1f textures (%.1f KB), %.1f KB sent; %zu live meshes",
      static_cast<unsigned long long>(sinceFrame), static_cast<unsigned long long>(g_frame), frames * 1000 / elapsed,
      elapsed / frames, c.draws / frames, c.kinds[0] / frames, c.kinds[1] / frames, c.kinds[2] / frames,
      c.kinds[3] / frames, per(c.capture), per(c.translate), per(c.geometry), per(c.vertexCapture),
      per(c.textureRead), per(c.present), per(c.send), c.meshes / frames, c.meshBytes / frames / 1024,
      c.textures / frames, c.textureBytes / frames / 1024, c.sentBytes / frames / 1024, g_meshes.size());
  c = {};
  since = now;
  sinceFrame = g_frame;
}

void present(uint32_t width, uint32_t height) {
  if (!g_active)
    return;
  reportPerformance();
  perf::Timer timer(perf::g_counters.present);
  Renderer &r = renderer();
  // The frame's cameras; a frame without a Main camera keeps the last one.
  if (g_cameras[size_t(CameraClass::Main)].set)
    g_lastMain = g_cameras[size_t(CameraClass::Main)];
  else if (g_lastMain.set && !g_instances.empty())
    g_cameras[size_t(CameraClass::Main)] = g_lastMain;
  static const remixapi_CameraType types[3] = {REMIXAPI_CAMERA_TYPE_WORLD, REMIXAPI_CAMERA_TYPE_VIEW_MODEL,
                                               REMIXAPI_CAMERA_TYPE_SKY};
  for (size_t i = 0; i < 3; ++i)
    if (g_cameras[i].set) {
      CameraDesc camera = {};
      camera.type = types[i];
      std::memcpy(camera.view, &g_cameras[i].view, sizeof(camera.view));
      std::memcpy(camera.projection, &g_cameras[i].projection, sizeof(camera.projection));
      r.setCamera(camera);
    }
  if (g_fogSet && std::memcmp(&g_fog, &g_sentFog, sizeof(g_fog)) != 0) {
    r.setFog(g_fog);
    g_sentFog = g_fog;
  }
  for (const Instance &instance : g_instances)
    r.drawInstance(instance.desc, instance.bones.data());
  for (const auto &[hash, lastFrame] : g_lights)
    if (lastFrame == g_frame)
      r.drawLight(hash);
  r.submitUi(width, height, g_uiVertices.data(), uint32_t(g_uiVertices.size()), g_uiIndices.data(),
             uint32_t(g_uiIndices.size()), g_uiCommands.data(), uint32_t(g_uiCommands.size()));

  // The first frames, then every 1000th: what the capture saw.
  if (g_frame <= 3 || g_frame % 1000 == 0) {
    const bool camera = g_cameras[size_t(CameraClass::Main)].set;
    log("frame %llu: %u forwarded, %u skipped, %u UI, %u world draws; %zu instances, %zu meshes, %zu lights; "
        "geometry memo %u hits, %u misses; %u shader draws captured, %u not%s%s%s",
        static_cast<unsigned long long>(g_frame), g_kinds[0], g_kinds[1], g_kinds[2], g_kinds[3],
        g_instances.size(), g_meshes.size(), g_lights.size(), g_memoHits, g_memoMisses, g_captured,
        g_captureFailures,
        camera ? "" : ", no camera", g_cameras[size_t(CameraClass::ViewModel)].set ? ", view model" : "",
        g_cameras[size_t(CameraClass::Sky)].set ? ", sky camera" : "");
    if (g_frame % 1000 == 0) {
      const RendererStatus s = r.status();
      log("backend: requested %u, active %u, supported 0x%x%s%s", s.requestedBackend, s.activeBackend,
          s.supportedBackends, s.reason[0] ? ": " : "", s.reason);
    }
  }
  std::fill(std::begin(g_kinds), std::end(g_kinds), 0u);
  g_memoHits = g_memoMisses = g_captured = g_captureFailures = 0;
  bool screenshot = !g_config.screenshot.empty() && g_frame == g_config.screenshotFrame;
  if (!g_config.screenshot.empty() && g_config.screenshotAfterActive) {
    // The frame the requested backend became active, looked for every 30th.
    static uint64_t activeSince = 0;
    if (!activeSince && g_frame % 30 == 0) {
      const RendererStatus s = r.status();
      const uint32_t wanted = g_config.backend == "pathtrace" ? 1 : g_config.backend == "raster" ? 0 : s.requestedBackend;
      if (s.started && s.requestedBackend == wanted && s.activeBackend == wanted)
        activeSince = g_frame;
    }
    screenshot = activeSince && g_frame == activeSince + g_config.screenshotAfterActive;
  }
  r.present(screenshot ? g_config.screenshot.c_str() : nullptr);

  g_instances.clear();
  g_uiVertices.clear();
  g_uiIndices.clear();
  g_uiCommands.clear();
  for (Camera &c : g_cameras)
    c.set = false;
  g_cameraFrame.endFrame();
  g_fogSet = false;
  collect();
  if (!g_config.hashLog.empty() && g_frame % 600 == 0)
    writeHashLog();
  ++g_frame;
  // The next frame's draws, described (openremix.drawLog).
  if (g_drawLog) {
    std::fclose(g_drawLog);
    g_drawLog = nullptr;
    log("draw log written: %s", g_config.drawLog.c_str());
  }
  if (!g_config.drawLog.empty() && g_frame == g_config.drawLogFrame)
    g_drawLog = std::fopen(g_config.drawLog.c_str(), "w");
}

void setFullscreen(bool fullscreen) {
  if (g_active)
    renderer().setFullscreen(fullscreen);
}

} // namespace openremix::d3d9::translator
