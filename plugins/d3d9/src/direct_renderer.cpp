#include "direct_renderer.h"

#include "formats.h"
#include "log.h"

#include <remix/remix_native_ext.h>
#include <remix/remix_usd_ext.h>

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace openremix::d3d9 {
namespace {

std::string utf8(const std::filesystem::path &path) {
  const std::wstring wide = path.wstring();
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(size > 0 ? size : 0), '\0');
  if (size > 0)
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), out.data(), size, nullptr, nullptr);
  return out;
}

// OpenRemix reports through SDL's log (SDL_Log): into the plugin's log.
void __cdecl sdlLog(void *, int, int, const char *message) { log("openremix: %s", message); }
void routeSdlLog() {
  using SetLogOutput = void(__cdecl *)(void(__cdecl *)(void *, int, int, const char *), void *);
  if (HMODULE sdl = GetModuleHandleW(L"SDL3.dll"))
    if (auto set = reinterpret_cast<SetLogOutput>(GetProcAddress(sdl, "SDL_SetLogOutputFunction")))
      set(sdlLog, nullptr);
}

wchar_t *texturePath(uint64_t hash, wchar_t (&path)[32]) {
  std::swprintf(path, 32, L"0x%llX", static_cast<unsigned long long>(hash));
  return path;
}

} // namespace

struct DirectRenderer::State {
  remixapi_Interface api = {};
  PFN_remixapi_GetBackendCapabilitiesEXT backendCapabilities = nullptr;
  bool active = false, failed = false;
  uint64_t presented = 0;

  struct Texture {
    D3DFORMAT format = D3DFMT_UNKNOWN;
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> packed;
    remixapi_TextureHandle handle = nullptr;
    bool created = false, uiRegistered = false, failed = false;
  };
  std::mutex textureLock;
  std::unordered_map<uint64_t, Texture> textures;
  std::unordered_map<uint64_t, remixapi_MaterialHandle> materials;
  std::unordered_map<uint64_t, remixapi_MeshHandle> meshes;
  std::unordered_map<uint64_t, remixapi_LightHandle> lights;

  // Logs a failed call the first frames only (they repeat every frame).
  void check(const char *call, remixapi_ErrorCode error) const {
    if (error != REMIXAPI_ERROR_CODE_SUCCESS && presented < 3)
      log("frame %llu: %s failed: %d", static_cast<unsigned long long>(presented), call, int(error));
  }

  // The runtime's texture for a hash, made on first use. False if unknown or
  // not convertible.
  bool ensureTexture(uint64_t hash) {
    std::lock_guard<std::mutex> lock(textureLock);
    auto it = textures.find(hash);
    if (it == textures.end() || it->second.failed)
      return false;
    Texture &t = it->second;
    if (t.created)
      return true;
    Converted converted;
    if (!convertLevel(t.format, t.width, t.height, t.packed.data(), true, converted)) {
      t.failed = true;
      log("texture 0x%016llX: format %u not supported", static_cast<unsigned long long>(hash), unsigned(t.format));
      return false;
    }
    remixapi_TextureInfoGenerateMipsEXT mips = {};
    mips.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_GENERATE_MIPS_EXT;
    mips.generate = 1;
    remixapi_TextureInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    info.pNext = &mips;
    info.hash = hash;
    info.width = t.width;
    info.height = t.height;
    info.depth = 1;
    info.mipLevels = 1;
    info.format = converted.format;
    info.data = converted.data.data();
    info.dataSize = converted.data.size();
    if (api.CreateTexture(&info, &t.handle) != REMIXAPI_ERROR_CODE_SUCCESS) {
      t.failed = true;
      log("texture 0x%016llX: CreateTexture failed", static_cast<unsigned long long>(hash));
      return false;
    }
    t.created = true;
    return true;
  }

  // The UI texture for a hash (RGBA8, its own id space), registered on first use.
  remixapi_UITextureHandle uiTexture(uint64_t hash) {
    std::lock_guard<std::mutex> lock(textureLock);
    auto it = textures.find(hash);
    if (it == textures.end())
      return 0;
    Texture &t = it->second;
    if (t.uiRegistered)
      return hash;
    Converted converted;
    if (!convertLevel(t.format, t.width, t.height, t.packed.data(), false, converted))
      return 0;
    std::vector<uint8_t> rgba;
    if (converted.rgba8)
      rgba = std::move(converted.data);
    else if (converted.format == REMIXAPI_FORMAT_BC1_RGBA_UNORM_EXT)
      rgba = decodeBC1(converted.data.data(), t.width, t.height);
    else
      rgba = decodeBC3(converted.data.data(), t.width, t.height);
    if (api.RegisterUITexture(hash, t.width, t.height, REMIXAPI_FORMAT_R8G8B8A8_UNORM, rgba.data(), rgba.size()) !=
        REMIXAPI_ERROR_CODE_SUCCESS)
      return 0;
    t.uiRegistered = true;
    return hash;
  }
};

DirectRenderer::DirectRenderer() : m(std::make_unique<State>()) {}
DirectRenderer::~DirectRenderer() = default;

bool DirectRenderer::start(HWND window, const Config &config, const std::filesystem::path &pluginDirectory,
                           const std::filesystem::path &gameDirectory) {
  if (m->active)
    return true;
  const std::filesystem::path dll = pluginDirectory / "openremix" / "remix.dll";
  // remix.dll's own dependencies (SDL3, OpenUSD, DLSS) sit next to it.
  if (!LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
    log("cannot load %s (error %lu)", utf8(dll).c_str(), GetLastError());
    m->failed = true;
    return false;
  }
  remixapi_HMODULE module = nullptr;
  if (remixapi_lib_loadRemixDllAndInitialize(dll.c_str(), &m->api, &module) != REMIXAPI_ERROR_CODE_SUCCESS) {
    log("remixapi_InitializeLibrary failed");
    m->failed = true;
    return false;
  }
  routeSdlLog();
  remixapi_StartupInfo startup = {};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
  startup.hwnd = window;
  if (const remixapi_ErrorCode error = m->api.Startup(&startup); error != REMIXAPI_ERROR_CODE_SUCCESS) {
    log("Startup on window %p failed: %d", static_cast<void *>(window), int(error));
    m->failed = true;
    return false;
  }
  m->backendCapabilities = reinterpret_cast<PFN_remixapi_GetBackendCapabilitiesEXT>(
      GetProcAddress(reinterpret_cast<HMODULE>(module), "remixapi_GetBackendCapabilitiesEXT"));
  if (!config.backend.empty())
    m->api.SetConfigVariable("rtx.native.backend", config.backend.c_str());
  m->api.SetConfigVariable("rtx.zUp", config.zUp ? "1" : "0");
  const std::string scale = std::to_string(metersPerUnit(config));
  m->api.SetConfigVariable("rtx.native.pathtrace.sceneScale", scale.c_str());
  for (const auto &[key, value] : config.passthrough)
    m->api.SetConfigVariable(key.c_str(), value.c_str());

  const std::filesystem::path mods = gameDirectory / "rtx-remix" / "mods";
  if (config.mods && std::filesystem::exists(mods)) {
    auto configure = reinterpret_cast<decltype(&remixapi_ConfigureUSDModsEXT)>(
        GetProcAddress(reinterpret_cast<HMODULE>(module), "remixapi_ConfigureUSDModsEXT"));
    if (configure) {
      const std::string directory = utf8(mods);
      remixapi_USDModsOptionsEXT options = {};
      options.structSize = sizeof(options);
      options.version = 1;
      options.directory = directory.c_str();
      options.hostMetersPerUnit = metersPerUnit(config);
      options.hostUpAxis = config.zUp ? 2 : 1;
      log("USD mods %s: %d", directory.c_str(), int(configure(&options)));
    }
  }
  m->active = true;
  log("OpenRemix started on window %p", static_cast<void *>(window));
  return true;
}

void DirectRenderer::shutdown() {
  if (!m->active)
    return;
  m->active = false;
  m->api.Shutdown();
}

void DirectRenderer::storeTexture(uint64_t hash, uint32_t format, uint32_t width, uint32_t height,
                                  const uint8_t *packed, size_t size) {
  std::lock_guard<std::mutex> lock(m->textureLock);
  State::Texture &t = m->textures[hash];
  if (t.created || t.uiRegistered || !t.packed.empty())
    return; // the same contents are already known
  t.format = D3DFORMAT(format);
  t.width = width;
  t.height = height;
  t.packed.assign(packed, packed + size);
}

void DirectRenderer::createMaterial(const MaterialDesc &d) {
  if (!m->active || m->materials.count(d.hash))
    return;
  wchar_t path[32];
  remixapi_MaterialInfoOpaqueEXT opaque = {};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.albedoConstant = {1, 1, 1};
  opaque.opacityConstant = 1;
  opaque.alphaTestType = 7;         // always pass; each draw's alpha state applies
  opaque.useDrawCallAlphaState = 1; // InstanceInfoBlendEXT
  remixapi_MaterialInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  info.pNext = &opaque;
  info.hash = d.hash;
  info.albedoTexture = d.texture && m->ensureTexture(d.texture) ? texturePath(d.texture, path) : nullptr;
  info.filterMode = d.filter;
  info.wrapModeU = d.wrapU;
  info.wrapModeV = d.wrapV;
  remixapi_MaterialHandle handle = nullptr;
  if (m->api.CreateMaterial(&info, &handle) != REMIXAPI_ERROR_CODE_SUCCESS)
    handle = nullptr;
  m->materials[d.hash] = handle;
}

void DirectRenderer::createMesh(uint64_t key, const Vertex *vertices, uint32_t vertexCount, const uint32_t *indices,
                                uint32_t indexCount, uint64_t material, const Skinning &skinning) {
  if (!m->active)
    return;
  remixapi_MeshInfoSurfaceTriangles surface = {};
  surface.vertices_values = vertices;
  surface.vertices_count = vertexCount;
  surface.indices_values = indices;
  surface.indices_count = indexCount;
  if (skinning.bonesPerVertex) {
    surface.skinning_hasvalue = 1;
    surface.skinning_value.bonesPerVertex = skinning.bonesPerVertex;
    surface.skinning_value.blendWeights_values = skinning.weights;
    surface.skinning_value.blendWeights_count = vertexCount * skinning.bonesPerVertex;
    surface.skinning_value.blendIndices_values = skinning.indices;
    surface.skinning_value.blendIndices_count = vertexCount * skinning.bonesPerVertex;
  }
  auto it = m->materials.find(material);
  surface.material = it != m->materials.end() ? it->second : nullptr;
  remixapi_MeshInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  info.hash = key;
  info.surfaces_values = &surface;
  info.surfaces_count = 1;
  remixapi_MeshHandle handle = nullptr;
  if (m->api.CreateMesh(&info, &handle) != REMIXAPI_ERROR_CODE_SUCCESS)
    handle = nullptr;
  m->meshes[key] = handle;
}

void DirectRenderer::destroyMesh(uint64_t key) {
  auto it = m->meshes.find(key);
  if (it == m->meshes.end())
    return;
  if (it->second && m->active)
    m->api.DestroyMesh(it->second);
  m->meshes.erase(it);
}

void DirectRenderer::createLight(uint64_t hash, const LightDesc &d) {
  if (!m->active)
    return;
  remixapi_LightInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  info.hash = hash;
  info.isDynamic = 1;
  info.radiance = {d.radiance[0], d.radiance[1], d.radiance[2]};
  remixapi_LightInfoSphereEXT sphere = {};
  remixapi_LightInfoDistantEXT distant = {};
  if (d.distant) {
    distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    distant.direction = {d.direction[0], d.direction[1], d.direction[2]};
    distant.angularDiameterDegrees = d.angularDiameter;
    distant.volumetricRadianceScale = 1;
    info.pNext = &distant;
  } else {
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position = {d.position[0], d.position[1], d.position[2]};
    sphere.radius = d.radius;
    sphere.volumetricRadianceScale = 1;
    if (d.shaped) {
      sphere.shaping_hasvalue = 1;
      sphere.shaping_value.direction = {d.direction[0], d.direction[1], d.direction[2]};
      sphere.shaping_value.coneAngleDegrees = d.coneAngle;
      sphere.shaping_value.coneSoftness = d.coneSoftness;
      sphere.shaping_value.focusExponent = d.focus;
    }
    info.pNext = &sphere;
  }
  remixapi_LightHandle handle = nullptr;
  if (m->api.CreateLight(&info, &handle) != REMIXAPI_ERROR_CODE_SUCCESS)
    handle = nullptr;
  m->lights[hash] = handle;
}

void DirectRenderer::destroyLight(uint64_t hash) {
  auto it = m->lights.find(hash);
  if (it == m->lights.end())
    return;
  if (it->second && m->active)
    m->api.DestroyLight(it->second);
  m->lights.erase(it);
}

void DirectRenderer::setCamera(const CameraDesc &d) {
  if (!m->active)
    return;
  remixapi_CameraInfo camera = {};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  camera.type = remixapi_CameraType(d.type);
  std::memcpy(camera.view, d.view, sizeof(camera.view));
  std::memcpy(camera.projection, d.projection, sizeof(camera.projection));
  m->check("SetupCamera", m->api.SetupCamera(&camera));
}

void DirectRenderer::drawInstance(const InstanceDesc &d, const remixapi_Transform *bones) {
  auto mesh = m->meshes.find(d.mesh);
  if (!m->active || mesh == m->meshes.end() || !mesh->second)
    return;
  remixapi_InstanceInfoBlendEXT blend = {};
  blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
  const BlendState &b = d.blend;
  blend.alphaTestEnabled = b.alphaTestEnabled;
  blend.alphaTestReferenceValue = uint8_t(b.alphaTestReference);
  blend.alphaTestCompareOp = b.alphaTestCompareOp;
  blend.alphaBlendEnabled = b.alphaBlendEnabled;
  blend.srcColorBlendFactor = b.srcColorBlendFactor;
  blend.dstColorBlendFactor = b.dstColorBlendFactor;
  blend.colorBlendOp = b.colorBlendOp;
  blend.textureColorArg1Source = b.textureColorArg1Source;
  blend.textureColorArg2Source = b.textureColorArg2Source;
  blend.textureColorOperation = b.textureColorOperation;
  blend.textureAlphaArg1Source = b.textureAlphaArg1Source;
  blend.textureAlphaArg2Source = b.textureAlphaArg2Source;
  blend.textureAlphaOperation = b.textureAlphaOperation;
  blend.tFactor = b.tFactor;
  blend.srcAlphaBlendFactor = b.srcAlphaBlendFactor;
  blend.dstAlphaBlendFactor = b.dstAlphaBlendFactor;
  blend.alphaBlendOp = b.alphaBlendOp;
  blend.writeMask = b.writeMask;
  void *chain = &blend;
  remixapi_InstanceInfoBoneTransformsEXT boneInfo = {};
  if (d.boneCount) {
    boneInfo.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
    boneInfo.pNext = chain;
    boneInfo.boneTransforms_values = bones;
    boneInfo.boneTransforms_count = d.boneCount;
    chain = &boneInfo;
  }
  remixapi_InstanceInfoReplacementHashEXT replacement = {};
  if (d.replacement) {
    replacement.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_REPLACEMENT_HASH_EXT;
    replacement.pNext = chain;
    replacement.meshHash = d.replacement;
    chain = &replacement;
  }
  remixapi_InstanceIdentityEXT identity = {};
  if (d.rasterOnly) {
    // Additive draws (glows, flares) have no surface to trace: rasterized
    // over the traced frame with their blend state.
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.pNext = chain;
    identity.classification = REMIXAPI_INSTANCE_RASTER_ONLY_EXT;
    identity.rasterVisible = 1;
    chain = &identity;
  }
  remixapi_InstanceInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  info.pNext = chain;
  info.categoryFlags = d.categories;
  info.mesh = mesh->second;
  std::memcpy(info.transform.matrix, d.transform, sizeof(info.transform.matrix));
  info.doubleSided = d.doubleSided;
  m->check("DrawInstance", m->api.DrawInstance(&info));
}

void DirectRenderer::drawLight(uint64_t hash) {
  auto it = m->lights.find(hash);
  if (m->active && it != m->lights.end() && it->second)
    m->api.DrawLightInstance(it->second);
}

void DirectRenderer::setFog(const FogDesc &d) {
  if (!m->active)
    return;
  remixapi_FogInfo fog = {};
  fog.sType = REMIXAPI_STRUCT_TYPE_FOG_INFO;
  fog.mode = d.mode;
  fog.color = {float((d.color >> 16) & 255) / 255, float((d.color >> 8) & 255) / 255, float(d.color & 255) / 255};
  fog.scale = d.scale;
  fog.end = d.end;
  fog.density = d.density;
  m->api.SetFogState(&fog);
}

void DirectRenderer::submitUi(uint32_t width, uint32_t height, const UiVertex *vertices, uint32_t vertexCount,
                              const uint32_t *indices, uint32_t indexCount, const UiCommand *commands,
                              uint32_t commandCount) {
  if (!m->active)
    return;
  std::vector<remixapi_UIDrawCommand> list(commandCount);
  for (uint32_t i = 0; i < commandCount; ++i) {
    list[i].textureId = commands[i].texture ? m->uiTexture(commands[i].texture) : 0;
    list[i].indexCount = commands[i].indexCount;
    list[i].indexOffset = commands[i].indexOffset;
    list[i].vertexOffset = commands[i].vertexOffset;
    list[i].flags = commands[i].flags;
  }
  remixapi_UIDrawList ui = {};
  ui.sType = REMIXAPI_STRUCT_TYPE_UI_DRAW_LIST;
  ui.displayWidth = width;
  ui.displayHeight = height;
  ui.pVertices = vertices;
  ui.vertexCount = vertexCount;
  ui.pIndices = indices;
  ui.indexCount = indexCount;
  ui.pCommands = list.data();
  ui.commandCount = commandCount;
  m->check("SubmitUIDrawList", m->api.SubmitUIDrawList(&ui));
}

void DirectRenderer::present(const char *screenshot) {
  if (!m->active)
    return;
  m->api.PumpEvents();
  if (screenshot && *screenshot)
    m->api.RequestPresentedScreenshot(screenshot);
  m->check("Present", m->api.Present(nullptr));
  ++m->presented;
}

void DirectRenderer::setFullscreen(bool fullscreen) {
  if (m->active)
    m->api.SetFullscreen(fullscreen);
}

RendererStatus DirectRenderer::status() {
  RendererStatus s = {};
  s.started = m->active;
  s.failed = m->failed;
  s.presented = m->presented;
  if (!m->active)
    return s;
  if (m->backendCapabilities) {
    remixapi_BackendCapabilitiesEXT backend = {};
    backend.structSize = sizeof(backend);
    if (m->backendCapabilities(&backend) == REMIXAPI_ERROR_CODE_SUCCESS) {
      s.requestedBackend = backend.requestedBackend;
      s.activeBackend = backend.activeBackend;
      s.supportedBackends = backend.supportedBackends;
      std::memcpy(s.reason, backend.unavailableReason, sizeof(s.reason));
    }
  }
  remixapi_WindowState window = {};
  if (m->api.GetWindowState && m->api.GetWindowState(&window) == REMIXAPI_ERROR_CODE_SUCCESS) {
    s.guiWantsKeyboard = window.guiWantsKeyboard;
    s.guiWantsMouse = window.guiWantsMouse;
  }
  s.menuOpen = m->api.GetUIState && m->api.GetUIState() != REMIXAPI_UI_STATE_NONE;
  return s;
}

bool DirectRenderer::alive() { return m->active; }

#if defined(_WIN64) && !defined(OPENREMIX_D3D9_HOST)
Renderer &renderer() {
  static DirectRenderer instance;
  return instance;
}
#endif

} // namespace openremix::d3d9
