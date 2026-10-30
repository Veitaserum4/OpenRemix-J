#pragma once
#include "config.h"

#include <windows.h>

#include <remix/remix_c.h>

#include <cstdint>
#include <filesystem>

// What the translator asks of OpenRemix, in plain structs keyed by the
// plugin's own 64-bit keys (texture and material hashes, mesh keys, light
// hashes): nothing waits for a handle, so every call can cross a process
// boundary without a reply. The translator owns every lifetime (it creates
// and destroys meshes and lights); the renderer only maps keys to the
// runtime's objects. DirectRenderer calls remix.dll in this process;
// RemoteRenderer (32-bit games) sends the calls to OpenRemixHost.exe.
namespace openremix::d3d9 {

using Vertex = remixapi_HardcodedVertex;
using UiVertex = remixapi_UIVertex;

// remixapi_InstanceInfoBlendEXT without its chain fields.
struct BlendState {
  uint32_t alphaTestEnabled, alphaTestReference, alphaTestCompareOp;
  uint32_t alphaBlendEnabled, srcColorBlendFactor, dstColorBlendFactor, colorBlendOp;
  uint32_t textureColorArg1Source, textureColorArg2Source, textureColorOperation;
  uint32_t textureAlphaArg1Source, textureAlphaArg2Source, textureAlphaOperation;
  uint32_t tFactor, srcAlphaBlendFactor, dstAlphaBlendFactor, alphaBlendOp, writeMask;
};

struct MaterialDesc {
  uint64_t hash;    // the material's key (its albedo texture's hash, as dxvk-remix keys materials)
  uint64_t texture; // albedo texture hash (0: none)
  uint8_t filter, wrapU, wrapV, pad;
};

// Fixed-function vertex blending: per vertex `bonesPerVertex` weights and
// indices into the instance's bone transforms.
struct Skinning {
  uint32_t bonesPerVertex = 0;
  const float *weights = nullptr;    // bonesPerVertex per vertex
  const uint32_t *indices = nullptr; // bonesPerVertex per vertex
};

struct InstanceDesc {
  uint64_t mesh;
  float transform[3][4]; // remixapi_Transform
  uint32_t categories;   // REMIXAPI_INSTANCE_CATEGORY_BIT_*
  BlendState blend;
  uint64_t replacement;  // USD mesh replacement key (0: none)
  uint8_t doubleSided, rasterOnly, pad[2];
  uint32_t boneCount;    // bone transforms that follow (InstanceBones)
};

struct LightDesc {
  uint32_t distant;            // 0: sphere (point or spot), 1: distant
  float radiance[3];
  float position[3], radius;   // sphere
  float direction[3];          // distant, or the spot's axis
  uint32_t shaped;             // spot
  float coneAngle, coneSoftness, focus; // degrees, cosine difference, exponent
  float angularDiameter;       // distant, degrees
};

struct CameraDesc {
  uint32_t type; // remixapi_CameraType
  float view[16], projection[16];
};

struct UiCommand {
  uint64_t texture; // texture hash (0: white)
  uint32_t indexCount, indexOffset;
  int32_t vertexOffset;
  uint32_t flags;   // REMIXAPI_UI_* flags
};

struct FogDesc {
  uint32_t mode; // D3DFOGMODE (0: off)
  uint32_t color; // D3DCOLOR
  float scale, end, density;
};

// What the runtime reports back (RemoteRenderer: a shared block the host keeps
// up to date, read without a round trip).
struct RendererStatus {
  uint32_t started, failed;
  uint32_t requestedBackend, activeBackend, supportedBackends;
  uint32_t guiWantsKeyboard, guiWantsMouse;
  uint64_t presented;
  char reason[256];
};

class Renderer {
public:
  virtual ~Renderer() = default;
  // The plugin's settings and folders, once, before anything else.
  virtual void prepare(const Config &, const std::filesystem::path & /*pluginDirectory*/,
                       const std::filesystem::path & /*gameDirectory*/) {}
  // Starts OpenRemix on `window` (the thread that presents calls it).
  virtual bool start(HWND window, const Config &config, const std::filesystem::path &pluginDirectory,
                     const std::filesystem::path &gameDirectory) = 0;
  virtual void shutdown() = 0;
  // Level 0 of a texture (formats.h packLevel), from any thread, also before
  // start; the runtime's texture is made when a material or UI uses it.
  virtual void storeTexture(uint64_t hash, uint32_t format, uint32_t width, uint32_t height, const uint8_t *packed,
                            size_t size) = 0;
  virtual void createMaterial(const MaterialDesc &material) = 0;
  virtual void createMesh(uint64_t key, const Vertex *vertices, uint32_t vertexCount, const uint32_t *indices,
                          uint32_t indexCount, uint64_t material, const Skinning &skinning) = 0;
  virtual void destroyMesh(uint64_t key) = 0;
  virtual void createLight(uint64_t hash, const LightDesc &light) = 0;
  virtual void destroyLight(uint64_t hash) = 0;
  virtual void setCamera(const CameraDesc &camera) = 0;
  // `bones`: instance.boneCount transforms (3x4).
  virtual void drawInstance(const InstanceDesc &instance, const remixapi_Transform *bones) = 0;
  virtual void drawLight(uint64_t hash) = 0;
  virtual void setFog(const FogDesc &fog) = 0;
  virtual void submitUi(uint32_t width, uint32_t height, const UiVertex *vertices, uint32_t vertexCount,
                        const uint32_t *indices, uint32_t indexCount, const UiCommand *commands,
                        uint32_t commandCount) = 0;
  // Ends the frame; `screenshot` (or empty) saves the presented frame.
  virtual void present(const char *screenshot) = 0;
  virtual void setFullscreen(bool fullscreen) = 0;
  virtual RendererStatus status() = 0;
  // Whether frames still reach OpenRemix (false: the game presents itself).
  virtual bool alive() = 0;
};

// This build's renderer: DirectRenderer (x64) or RemoteRenderer (x86).
Renderer &renderer();

} // namespace openremix::d3d9
