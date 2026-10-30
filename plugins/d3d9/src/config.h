#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// The game's RTX Remix configuration (dxvk.conf, user.conf, rtx.conf) as far
// as the plugin applies it, plus its own openremix.conf.
namespace openremix::d3d9 {

// Geometry hash rule components (rtx.geometryAssetHashRuleString).
enum : uint32_t {
  kHashPositions = 1,
  kHashTexcoords = 2,
  kHashIndices = 4,
  kHashDescriptor = 8,
  kHashLayout = 16,
};
// The components a rule string names ("positions,indices,geometrydescriptor").
uint32_t parseHashRule(const std::string &rule);

struct Config {
  // Texture hash -> REMIXAPI_INSTANCE_CATEGORY_BIT_* (rtx.*Textures lists).
  std::unordered_map<uint64_t, uint32_t> categories;
  std::unordered_set<uint64_t> uiTextures, ignoreLights, lightmapTextures;

  bool orthographicIsUI = true;
  bool preTransformedVerticesIsUI = true;
  bool enableAlphaBlend = true, enableAlphaTest = true;
  bool zUp = false, leftHanded = false;
  float sceneScale = 1; // rtx.sceneScale: 1 cm per game unit

  // Hashing (dxvk-remix rtx_hashing.h, rtx_options.h).
  uint32_t assetHashRule = kHashPositions | kHashIndices | kHashDescriptor; // rtx.geometryAssetHashRuleString
  bool useObsoleteHashOnTextureUpload = false; // XXH64 for textures filled by locks
  bool recomputeTextureHashOnWrite = false;    // a texture's hash follows its later writes
  bool allowCubemaps = false;

  // Vertex capture (dxvk-remix d3d9_rtx.h).
  bool useVertexCapture = true;            // vertex-shader draws are captured
  bool useVertexCapturedNormals = true;    // their input normals are kept
  bool useVertexCapturedTexcoords = false; // their output texcoords replace the input ones
  bool useWorldMatricesForShaders = true;  // D3DTS_WORLD places them

  // Capture heuristics (dxvk-remix D3D9Rtx and the Remix Plus translator).
  uint32_t drawCallRangeFirst = 0, drawCallRangeLast = UINT32_MAX; // rtx.drawCallRange
  uint32_t skyDrawcallIdThreshold = 0; // untextured draws before this one are sky
  float skyMinZThreshold = 1;          // a viewport from this depth on draws sky
  bool viewModelEnable = false;        // rtx.viewModel.enable
  float viewModelMaxZThreshold = 0;    // a viewport up to this depth draws the view model
  uint32_t fusedWorldViewMode = 0;     // rtx.fusedWorldViewMode: 0 none, 1 view, 2 world

  // Every rtx.* key the plugin does not consume, in file order, for
  // SetConfigVariable (OpenRemix takes the ones it knows).
  std::vector<std::pair<std::string, std::string>> passthrough;

  // openremix.conf (plugin settings; also accepted in the files above).
  std::string backend;           // openremix.backend: raster | pathtrace (runtime default when empty)
  bool forwardDraws = false;     // openremix.forwardDraws: also rasterize captured draws on the real device
  bool mods = true;              // openremix.mods: load rtx-remix/mods
  bool log = true;               // openremix.log: write openremix-d3d9.log
  uint32_t bridgeMemory = 64;    // openremix.bridgeMemory: MB of the 32-bit bridge's ring
  uint32_t bridgeFramesAhead = 1; // openremix.bridgeFramesAhead: frames a 32-bit game may run ahead of the host
  std::string screenshot;        // openremix.screenshot: a file (.png) to save the presented frame to
  uint32_t screenshotFrame = 60; // openremix.screenshotFrame: at this frame
  // openremix.screenshotAfterActive: instead, this many frames after the
  // requested backend became active (a path tracer prepares for a while).
  uint32_t screenshotAfterActive = 0;
  // openremix.hashLog: a file listing the texture hashes and mesh replacement
  // keys the game's draws had, rewritten every 600 frames (to check them
  // against a mod's mesh_ and mat_ names).
  std::string hashLog;
};

// Applies one file's `key = value` lines on top of `config`.
void parseConfig(const std::string &text, Config &config);
// Layers dxvk.conf, the game's built-in profile (profiles.h, matched against
// `executable`), user.conf, rtx.conf and openremix.conf from `directory`
// (later ones win), honouring DXVK_CONFIG_FILE and DXVK_RTX_CONFIG_FILE.
Config loadConfig(const std::filesystem::path &directory, const std::filesystem::path &executable = {});

// Metres per game unit for OpenRemix (rtx.native.pathtrace.sceneScale).
inline float metersPerUnit(const Config &c) { return c.sceneScale > 0 ? 0.01f / c.sceneScale : 0.01f; }

} // namespace openremix::d3d9
