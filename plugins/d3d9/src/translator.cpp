#include "translator.h"

#include "camera.h"
#include "hash.h"
#include "log.h"
#include "renderer.h"
#include "vertex.h"

#include <remix/remix_native_ext.h>

#include <algorithm>
#include <cmath>
#include <cstring>
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
std::vector<UiVertex> g_uiVertices;
std::vector<uint32_t> g_uiIndices;
std::vector<UiCommand> g_uiCommands;
uint32_t g_kinds[4] = {}; // draws per DrawKind this frame

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
uint64_t lightHash(const D3DLIGHT9 &l) { return XXH3_64bits(&l, sizeof(l)); }

void addLight(const D3DLIGHT9 &l) {
  const uint64_t hash = lightHash(l);
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
// covers (UP draws).
uint64_t memoKey(const Draw &d) {
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
  return XXH3_64bits_withSeed(draw, sizeof(draw), key) | 1;
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
  auto g = std::make_shared<Geometry>();
  Triangles triangles;
  bool degenerate = false;
  if (!triangulate(d, triangles) || !decode(d, triangles, g->mesh))
    return nullptr;
  g->asset = geometryHash(d, g_config.assetHashRule, &degenerate);
  if (degenerate)
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
struct Skin {
  uint32_t bonesPerVertex = 0;
  std::vector<float> weights;
  std::vector<uint32_t> indices;
  std::vector<remixapi_Transform> bones;
  int single = -1; // the one matrix used
  D3DMATRIX singleMatrix;
};

bool skinning(const Draw &d, const Mesh &m, Skin &out) {
  const DWORD blend = d.renderStates[D3DRS_VERTEXBLEND];
  if (blend == D3DVBF_DISABLE || blend == D3DVBF_TWEENING || !d.device)
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
  out.bones.resize(high + 1);
  for (uint32_t i = 0; i <= high; ++i) {
    D3DMATRIX world;
    d.device->GetTransform(D3DTS_WORLDMATRIX(i), &world);
    float t[3][4];
    transformOf(world, t);
    std::memcpy(out.bones[i].matrix, t, sizeof(t));
  }
  return true;
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

bool worldDraw(const Draw &d) {
  const std::shared_ptr<const Geometry> geometry = geometryFor(d);
  if (!geometry)
    return false;
  uint32_t categories = 0;
  if (d.texture0)
    if (auto it = g_config.categories.find(d.texture0); it != g_config.categories.end())
      categories = it->second;
  const bool sky = skyDraw(d, g_config, categories);
  Skin skin;
  const bool skinned = skinning(d, geometry->mesh, skin);
  bool updatesCamera;
  const CameraClass camera = classifyCamera(d, g_config, sky, skinned, g_cameraFrame, updatesCamera);
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
  if (d.renderStates[D3DRS_LIGHTING] &&
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
  }

  Instance instance = {};
  InstanceDesc &desc = instance.desc;
  desc.mesh = key;
  // Skinned vertices go to the world through their bones.
  static const D3DMATRIX identity = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
  transformOf(skin.single >= 0 ? skin.singleMatrix : skinMesh ? identity : d.world, desc.transform);
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
  // Double-sided whatever D3DRS_CULLMODE says: the raster backend takes
  // clockwise faces as front (D3D's screen-space rule), the path tracer
  // decides in object space, where a left-handed D3D world's are reversed.
  desc.doubleSided = 1;
  // Additive draws (glows, flares) have no surface to trace.
  desc.rasterOnly = desc.blend.alphaBlendEnabled && desc.blend.dstColorBlendFactor == 1;
  g_instances.push_back(std::move(instance));
  if (d.renderStates[D3DRS_LIGHTING])
    for (uint32_t i = 0; i < d.lightCount; ++i)
      addLight(d.lights[i].light);
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
  if (!g_active)
    return;
  g_active = false;
  renderer().shutdown();
}

void textureFilled(uint64_t hash, D3DFORMAT format, uint32_t width, uint32_t height,
                   const std::vector<uint8_t> &packed) {
  renderer().storeTexture(hash, uint32_t(format), width, height, packed.data(), packed.size());
}

bool draw(const Draw &d, DrawKind kind) {
  if (!g_active)
    return false;
  const bool taken = (kind == DrawKind::World && worldDraw(d)) || (kind == DrawKind::Ui && uiDraw(d));
  // A draw the translator could not read stays the real device's.
  ++g_kinds[size_t(taken || kind == DrawKind::Skip ? kind : DrawKind::Forward)];
  return taken;
}

void present(uint32_t width, uint32_t height) {
  if (!g_active)
    return;
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
        "geometry memo %u hits, %u misses%s%s%s",
        static_cast<unsigned long long>(g_frame), g_kinds[0], g_kinds[1], g_kinds[2], g_kinds[3],
        g_instances.size(), g_meshes.size(), g_lights.size(), g_memoHits, g_memoMisses,
        camera ? "" : ", no camera", g_cameras[size_t(CameraClass::ViewModel)].set ? ", view model" : "",
        g_cameras[size_t(CameraClass::Sky)].set ? ", sky camera" : "");
    if (g_frame % 1000 == 0) {
      const RendererStatus s = r.status();
      log("backend: requested %u, active %u, supported 0x%x%s%s", s.requestedBackend, s.activeBackend,
          s.supportedBackends, s.reason[0] ? ": " : "", s.reason);
    }
  }
  std::fill(std::begin(g_kinds), std::end(g_kinds), 0u);
  g_memoHits = g_memoMisses = 0;
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
  ++g_frame;
}

void setFullscreen(bool fullscreen) {
  if (g_active)
    renderer().setFullscreen(fullscreen);
}

} // namespace openremix::d3d9::translator
