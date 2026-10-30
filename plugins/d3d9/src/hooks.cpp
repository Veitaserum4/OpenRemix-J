// Define the D3D9 interface GUIDs (IID_IDirect3DTexture9) in this unit.
#include <initguid.h>

#include "hooks.h"

#include "classify.h"
#include "formats.h"
#include "hash.h"
#include "input_hooks.h"
#include "log.h"
#include "perf.h"
#include "renderer.h"
#include "translator.h"
#include "vertex_capture.h"
#include "vertex.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include "xxhash.h"

namespace openremix::d3d9::hooks {
namespace {

// Vtable slots.
enum : unsigned {
  // IUnknown
  kRelease = 2,
  // IDirect3D9 / IDirect3D9Ex
  kCreateDevice = 16,
  kCreateDeviceEx = 20,
  // IDirect3DDevice9 / IDirect3DDevice9Ex
  kGetCreationParameters = 9,
  kCreateAdditionalSwapChain = 13,
  kGetSwapChain = 14,
  kReset = 16,
  kPresent = 17,
  kCreateTexture = 23,
  kCreateVertexBuffer = 26,
  kCreateIndexBuffer = 27,
  kUpdateSurface = 30,
  kUpdateTexture = 31,
  kSetLight = 51,
  kLightEnable = 53,
  kSetSoftwareVertexProcessing = 77,
  kGetSoftwareVertexProcessing = 78,
  kDrawPrimitive = 81,
  kDrawIndexedPrimitive = 82,
  kDrawPrimitiveUP = 83,
  kDrawIndexedPrimitiveUP = 84,
  kCreateVertexDeclaration = 86,
  kCreateVertexShader = 91,
  kCreateQuery = 118,
  kPresentEx = 121,
  kResetEx = 132,
  // IDirect3DSwapChain9
  kSwapChainPresent = 3,
  // IDirect3DQuery9
  kQueryIssue = 6,
  // IDirect3DVertexBuffer9 / IDirect3DIndexBuffer9
  kBufferLock = 11,
  kBufferUnlock = 12,
  // IDirect3DTexture9
  kTextureGetSurfaceLevel = 18,
  kTextureLockRect = 19,
  kTextureUnlockRect = 20,
  // IDirect3DSurface9
  kSurfaceLockRect = 13,
  kSurfaceUnlockRect = 14,
};

Environment g_env;

// --- Vtable patching --------------------------------------------------------
// The original function of each patched (vtable, slot): objects of different
// classes may share a hook while having their own originals.
std::shared_mutex g_slotLock;
std::unordered_map<uint64_t, void *> g_originals;

uint64_t slotKey(void **vtable, unsigned slot) { return uint64_t(reinterpret_cast<uintptr_t>(vtable)) << 8 | slot; }
void **vtableOf(void *object) { return *static_cast<void ***>(object); }

// Patches `slot` of `object`'s vtable once.
void hook(void *object, unsigned slot, void *function) {
  void **vtable = vtableOf(object);
  std::unique_lock<std::shared_mutex> lock(g_slotLock);
  if (g_originals.count(slotKey(vtable, slot)))
    return;
  if (vtable[slot] == function)
    return; // another vtable's patch already put it here (shared vtable memory)
  DWORD old;
  VirtualProtect(&vtable[slot], sizeof(void *), PAGE_READWRITE, &old);
  g_originals[slotKey(vtable, slot)] = vtable[slot];
  vtable[slot] = function;
  VirtualProtect(&vtable[slot], sizeof(void *), old, &old);
}

template <class Fn> Fn original(void *object, unsigned slot) {
  std::shared_lock<std::shared_mutex> lock(g_slotLock);
  auto it = g_originals.find(slotKey(vtableOf(object), slot));
  return it == g_originals.end() ? nullptr : reinterpret_cast<Fn>(it->second);
}

// --- Device-wide state ------------------------------------------------------
HWND g_gameWindow = nullptr, g_hiddenWindow = nullptr;
bool g_fullscreen = false;
uint32_t g_backBufferWidth = 0, g_backBufferHeight = 0;
IDirect3DSurface9 *g_backBuffer = nullptr; // identity only
bool g_started = false, g_startFailed = false;
std::atomic<int> g_openQueries{0};
uint32_t g_drawId = 0; // draws this frame
// Set on a thread while the plugin makes its own D3D9 calls (InternalCalls).
thread_local bool t_internal = false;

// Vertex shaders' bytecode (vertex capture).
std::mutex g_shaderLock;
std::unordered_map<const void *, std::shared_ptr<const ShaderCode>> g_shaders;

std::mutex g_lightLock;
std::unordered_map<DWORD, D3DLIGHT9> g_lights;
// The light state changed since the enabled lights were last collected (set
// again each frame: each frame lights what is enabled).
std::atomic<bool> g_lightsDirty{true};

std::mutex g_declLock;

// Textures: their dxvk-remix hash, taken when a draw first uses the texture
// after it was written (dxvk-remix hashes at the first upload, after the
// game's writes), and kept (rtx.recomputeTextureHashOnWrite: retaken).
struct TextureState {
  uint64_t hash = 0;
  bool written = false; // level 0 was written since the hash was taken
};
std::mutex g_textureLock;
std::unordered_map<const void *, TextureState> g_textures;

// --- Buffer shadows ---------------------------------------------------------
// CPU copies of vertex and index buffers. A writable lock hands the game its
// range of the shadow and Unlock copies that range into the real mapping
// (locked GPU memory is write-combined: reading it back crawls).
struct Shadow {
  std::vector<uint8_t> data;
  uint32_t version = 1; // bumped by every write (geometry memoization keys)
  bool is32 = false;
  uint8_t *lockPointer = nullptr;
  uint32_t lockOffset = 0, lockSize = 0;
  bool lockReadOnly = false;
};
std::mutex g_shadowLock;
std::unordered_map<const void *, Shadow> g_shadows;

using ReleaseFn = ULONG(STDMETHODCALLTYPE *)(IUnknown *);
using LockFn = HRESULT(STDMETHODCALLTYPE *)(void *, UINT, UINT, void **, DWORD);
using UnlockFn = HRESULT(STDMETHODCALLTYPE *)(void *);

ULONG STDMETHODCALLTYPE bufferRelease(IUnknown *self) {
  ReleaseFn fn = original<ReleaseFn>(self, kRelease);
  const ULONG references = fn ? fn(self) : 0;
  if (references == 0 && !t_internal) {
    std::lock_guard<std::mutex> lock(g_shadowLock);
    g_shadows.erase(self);
  }
  return references;
}

HRESULT STDMETHODCALLTYPE bufferLock(void *self, UINT offset, UINT size, void **data, DWORD flags) {
  LockFn fn = original<LockFn>(self, kBufferLock);
  const HRESULT hr = fn ? fn(self, offset, size, data, flags) : D3DERR_INVALIDCALL;
  // The plugin's own buffers (vertex capture) have no shadow; the capture
  // runs while the shadows are locked.
  if (FAILED(hr) || !data || !*data || t_internal)
    return hr;
  std::lock_guard<std::mutex> lock(g_shadowLock);
  auto it = g_shadows.find(self);
  if (it == g_shadows.end())
    return hr;
  Shadow &s = it->second;
  s.lockPointer = static_cast<uint8_t *>(*data);
  s.lockOffset = offset;
  s.lockSize = size ? size : uint32_t(s.data.size()) - std::min<uint32_t>(offset, uint32_t(s.data.size()));
  s.lockReadOnly = (flags & D3DLOCK_READONLY) != 0;
  if (!s.lockReadOnly && offset < s.data.size() && s.lockSize <= s.data.size() - offset)
    *data = s.data.data() + offset;
  return hr;
}

HRESULT STDMETHODCALLTYPE bufferUnlock(void *self) {
  if (!t_internal) {
    std::lock_guard<std::mutex> lock(g_shadowLock);
    auto it = g_shadows.find(self);
    if (it != g_shadows.end()) {
      Shadow &s = it->second;
      if (s.lockPointer && !s.lockReadOnly && s.lockOffset < s.data.size() &&
          s.lockSize <= s.data.size() - s.lockOffset)
        std::memcpy(s.lockPointer, s.data.data() + s.lockOffset, s.lockSize);
      if (s.lockPointer && !s.lockReadOnly)
        ++s.version;
      s.lockPointer = nullptr;
    }
  }
  UnlockFn fn = original<UnlockFn>(self, kBufferUnlock);
  return fn ? fn(self) : D3DERR_INVALIDCALL;
}

void trackBuffer(void *buffer, UINT length, bool is32) {
  hook(buffer, kRelease, reinterpret_cast<void *>(&bufferRelease));
  hook(buffer, kBufferLock, reinterpret_cast<void *>(&bufferLock));
  hook(buffer, kBufferUnlock, reinterpret_cast<void *>(&bufferUnlock));
  std::lock_guard<std::mutex> lock(g_shadowLock);
  Shadow &s = g_shadows[buffer];
  s = Shadow();
  s.data.resize(length);
  s.is32 = is32;
}

// --- Textures ---------------------------------------------------------------
using TextureUnlockRectFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DTexture9 *, UINT);
using SurfaceUnlockRectFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DSurface9 *);
using GetSurfaceLevelFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DTexture9 *, UINT, IDirect3DSurface9 **);

// Set while readLevelZero locks a texture: the runtime may unlock a
// texture's level through its surface, which is hooked too.
thread_local bool t_reading = false;

// Reads level 0 of a lockable texture back: its hash, and its contents for
// OpenRemix. `obsolete`: the hash of a lock upload with
// rtx.useObsoleteHashOnTextureUpload (XXH64). 0 if it cannot be read.
uint64_t readLevelZero(IDirect3DTexture9 *texture, bool obsolete) {
  D3DSURFACE_DESC desc;
  if (t_reading || FAILED(texture->GetLevelDesc(0, &desc)) ||
      (desc.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)))
    return 0;
  FormatLayout layout;
  if (!formatLayout(desc.Format, layout))
    return 0;
  auto unlockRect = original<TextureUnlockRectFn>(texture, kTextureUnlockRect);
  if (!unlockRect)
    return 0;
  t_reading = true;
  D3DLOCKED_RECT locked;
  if (FAILED(texture->LockRect(0, &locked, nullptr, D3DLOCK_READONLY))) {
    t_reading = false;
    return 0;
  }
  std::vector<uint8_t> packed = packLevel(desc.Format, desc.Width, desc.Height,
                                          static_cast<const uint8_t *>(locked.pBits), uint32_t(locked.Pitch));
  unlockRect(texture, 0);
  t_reading = false;
  if (packed.empty())
    return 0;
  const uint64_t hash = textureHash(packed.data(), packed.size(), obsolete);
  translator::textureFilled(hash, desc.Format, desc.Width, desc.Height, packed);
  return hash;
}

// The game wrote level 0 of a texture.
void textureWritten(const void *texture) {
  std::lock_guard<std::mutex> lock(g_textureLock);
  g_textures[texture].written = true;
}

// The 2D texture whose level 0 `surface` is (not AddRef'd), or nullptr.
IDirect3DTexture9 *levelZeroTexture(IDirect3DSurface9 *surface) {
  IDirect3DTexture9 *texture = nullptr;
  if (FAILED(surface->GetContainer(IID_IDirect3DTexture9, reinterpret_cast<void **>(&texture))) || !texture)
    return nullptr;
  texture->Release(); // the texture outlives this use of its surface
  D3DSURFACE_DESC level0, mine;
  if (FAILED(texture->GetLevelDesc(0, &level0)) || FAILED(surface->GetDesc(&mine)) || level0.Width != mine.Width ||
      level0.Height != mine.Height)
    return nullptr;
  return texture;
}

HRESULT STDMETHODCALLTYPE textureUnlockRect(IDirect3DTexture9 *self, UINT level) {
  auto fn = original<TextureUnlockRectFn>(self, kTextureUnlockRect);
  const HRESULT hr = fn ? fn(self, level) : D3DERR_INVALIDCALL;
  if (SUCCEEDED(hr) && level == 0 && !t_reading)
    textureWritten(self);
  return hr;
}

HRESULT STDMETHODCALLTYPE surfaceUnlockRect(IDirect3DSurface9 *self) {
  auto fn = original<SurfaceUnlockRectFn>(self, kSurfaceUnlockRect);
  const HRESULT hr = fn ? fn(self) : D3DERR_INVALIDCALL;
  if (SUCCEEDED(hr) && !t_reading)
    if (IDirect3DTexture9 *texture = levelZeroTexture(self))
      textureWritten(texture);
  return hr;
}

HRESULT STDMETHODCALLTYPE textureGetSurfaceLevel(IDirect3DTexture9 *self, UINT level, IDirect3DSurface9 **out) {
  auto fn = original<GetSurfaceLevelFn>(self, kTextureGetSurfaceLevel);
  const HRESULT hr = fn ? fn(self, level, out) : D3DERR_INVALIDCALL;
  // Uploads through a level's surface (D3DX does this).
  if (SUCCEEDED(hr) && out && *out && level == 0)
    hook(*out, kSurfaceUnlockRect, reinterpret_cast<void *>(&surfaceUnlockRect));
  return hr;
}

void hookTexture(IDirect3DTexture9 *texture) {
  hook(texture, kTextureGetSurfaceLevel, reinterpret_cast<void *>(&textureGetSurfaceLevel));
  hook(texture, kTextureUnlockRect, reinterpret_cast<void *>(&textureUnlockRect));
}

// The hash of a texture a draw uses, taken now if it was written and has
// none yet (or follows writes: rtx.recomputeTextureHashOnWrite).
uint64_t textureHashOf(IDirect3DBaseTexture9 *texture) {
  {
    std::lock_guard<std::mutex> lock(g_textureLock);
    auto it = g_textures.find(texture);
    if (it == g_textures.end())
      return 0;
    TextureState &t = it->second;
    if (!t.written || (t.hash && !g_env.config.recomputeTextureHashOnWrite))
      return t.hash;
    t.written = false;
  }
  if (texture->GetType() != D3DRTYPE_TEXTURE)
    return 0;
  perf::Timer timer(perf::g_counters.textureRead);
  const uint64_t hash =
      readLevelZero(static_cast<IDirect3DTexture9 *>(texture), g_env.config.useObsoleteHashOnTextureUpload);
  std::lock_guard<std::mutex> lock(g_textureLock);
  if (hash)
    g_textures[texture].hash = hash;
  return g_textures[texture].hash;
}

// --- Window and presentation parameters ------------------------------------
LRESULT CALLBACK hiddenProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  return DefWindowProcW(window, message, w, l);
}

// The window the real device renders into: never shown (OpenRemix presents
// into the game's own, from this process or, for a 32-bit game, from
// OpenRemixHost.exe).
HWND hiddenWindow() {
  if (g_hiddenWindow)
    return g_hiddenWindow;
  WNDCLASSW wc = {};
  wc.lpfnWndProc = hiddenProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"OpenRemixD3D9Device";
  RegisterClassW(&wc);
  g_hiddenWindow = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr,
                                   wc.hInstance, nullptr);
  return g_hiddenWindow;
}

// The game's parameters, rewritten for a windowed device on the hidden
// window. Remembers the game's window, size and fullscreen request.
D3DPRESENT_PARAMETERS deviceParameters(HWND focus, const D3DPRESENT_PARAMETERS &game) {
  g_gameWindow = game.hDeviceWindow ? game.hDeviceWindow : focus;
  g_fullscreen = !game.Windowed;
  D3DPRESENT_PARAMETERS pp = game;
  if (!pp.BackBufferWidth || !pp.BackBufferHeight) {
    RECT client = {};
    GetClientRect(g_gameWindow, &client);
    pp.BackBufferWidth = std::max<LONG>(client.right - client.left, 1);
    pp.BackBufferHeight = std::max<LONG>(client.bottom - client.top, 1);
  }
  g_backBufferWidth = pp.BackBufferWidth;
  g_backBufferHeight = pp.BackBufferHeight;
  pp.Windowed = TRUE;
  pp.hDeviceWindow = hiddenWindow();
  pp.FullScreen_RefreshRateInHz = 0;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  return pp;
}

// What D3D wrote back into the parameters, with the game's own window,
// windowed state, refresh rate and interval.
void returnParameters(const D3DPRESENT_PARAMETERS &device, D3DPRESENT_PARAMETERS &game) {
  const D3DPRESENT_PARAMETERS mine = game;
  game = device;
  game.hDeviceWindow = mine.hDeviceWindow;
  game.Windowed = mine.Windowed;
  game.FullScreen_RefreshRateInHz = mine.FullScreen_RefreshRateInHz;
  game.PresentationInterval = mine.PresentationInterval;
}

void refreshBackBuffer(IDirect3DDevice9 *device) {
  IDirect3DSurface9 *surface = nullptr;
  g_backBuffer = nullptr;
  if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &surface)) && surface) {
    g_backBuffer = surface;
    surface->Release();
  }
}

// --- Frames -----------------------------------------------------------------
// OpenRemix starts on the thread that presents (its window calls must stay
// on the thread that started it).
bool ensureStarted() {
  if (g_started || g_startFailed)
    return g_started;
  g_started = translator::start(g_gameWindow, g_env.config, g_env.pluginDirectory, g_env.gameDirectory);
  g_startFailed = !g_started;
  if (g_started && g_fullscreen)
    translator::setFullscreen(true);
  if (g_startFailed)
    log("OpenRemix did not start: the real device presents instead");
  return g_started;
}

// Whether this frame goes to OpenRemix: it started and still takes frames
// (a 32-bit game's host may have stopped; the real device presents then).
bool presenting() { return ensureStarted() && translator::active(); }

// The game presented: the frame goes to OpenRemix.
void endFrame() {
  translator::present(g_backBufferWidth, g_backBufferHeight);
  g_drawId = 0;
  g_lightsDirty = true;
  input::setMenuOpen(renderer().status().menuOpen != 0);
  // Modules the game loaded since (rarely any after its first frames).
  static uint32_t frames = 0;
  if (++frames % 600 == 0)
    input::hook();
}

using PresentFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
using PresentExFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9Ex *, const RECT *, const RECT *, HWND,
                                                 const RGNDATA *, DWORD);
using SwapChainPresentFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DSwapChain9 *, const RECT *, const RECT *, HWND,
                                                        const RGNDATA *, DWORD);

HRESULT STDMETHODCALLTYPE present(IDirect3DDevice9 *self, const RECT *source, const RECT *dest, HWND window,
                                  const RGNDATA *dirty) {
  if (presenting()) {
    endFrame();
    return D3D_OK;
  }
  // Without OpenRemix the game still shows, through the real device.
  auto fn = original<PresentFn>(self, kPresent);
  return fn ? fn(self, source, dest, window ? window : g_gameWindow, dirty) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE presentEx(IDirect3DDevice9Ex *self, const RECT *source, const RECT *dest, HWND window,
                                    const RGNDATA *dirty, DWORD flags) {
  if (presenting()) {
    endFrame();
    return D3D_OK;
  }
  auto fn = original<PresentExFn>(self, kPresentEx);
  return fn ? fn(self, source, dest, window ? window : g_gameWindow, dirty, flags) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE swapChainPresent(IDirect3DSwapChain9 *self, const RECT *source, const RECT *dest,
                                           HWND window, const RGNDATA *dirty, DWORD flags) {
  if (presenting()) {
    endFrame();
    return D3D_OK;
  }
  auto fn = original<SwapChainPresentFn>(self, kSwapChainPresent);
  return fn ? fn(self, source, dest, window ? window : g_gameWindow, dirty, flags) : D3DERR_INVALIDCALL;
}

// --- Draws ------------------------------------------------------------------
// A vertex declaration's elements and their hash.
struct Declaration {
  std::vector<D3DVERTEXELEMENT9> elements;
  uint64_t hash = 0;
};
std::unordered_map<DWORD, Declaration> g_fvfDeclarations;
std::unordered_map<IDirect3DVertexDeclaration9 *, Declaration> g_declarations;

Declaration makeDeclaration(std::vector<D3DVERTEXELEMENT9> elements) {
  Declaration d;
  d.hash = XXH3_64bits(elements.data(), elements.size() * sizeof(D3DVERTEXELEMENT9));
  d.elements = std::move(elements);
  return d;
}

// The device state a draw needs, read through the Get* calls (state blocks
// change it without passing the setters).
// A shader draw's constants (vertex capture), alive while the draw is captured.
struct ShaderState {
  std::shared_ptr<const ShaderCode> code;
  float f[256 * 4];
  int i[16 * 4];
  BOOL b[16];
};

bool gatherState(IDirect3DDevice9 *dev, Draw &d, Declaration &declaration, ShaderState &shaderState) {
  IDirect3DVertexShader9 *shader = nullptr;
  if (SUCCEEDED(dev->GetVertexShader(&shader)) && shader) {
    d.vertexShader = true;
    if (g_env.config.useVertexCapture) {
      {
        std::lock_guard<std::mutex> lock(g_shaderLock);
        if (auto it = g_shaders.find(shader); it != g_shaders.end())
          shaderState.code = it->second;
      }
      if (shaderState.code) {
        dev->GetVertexShaderConstantF(0, shaderState.f, 256);
        dev->GetVertexShaderConstantI(0, shaderState.i, 16);
        dev->GetVertexShaderConstantB(0, shaderState.b, 16);
        d.shader = shaderState.code.get();
        d.constantsF = shaderState.f;
        d.constantsI = shaderState.i;
        d.constantsB = shaderState.b;
        uint64_t hash = XXH3_64bits(shaderState.f, sizeof(shaderState.f));
        hash = XXH3_64bits_withSeed(shaderState.i, sizeof(shaderState.i), hash);
        d.constantsHash = XXH3_64bits_withSeed(shaderState.b, sizeof(shaderState.b), hash);
      }
    }
    shader->Release();
  }
  IDirect3DPixelShader9 *pixel = nullptr;
  if (SUCCEEDED(dev->GetPixelShader(&pixel)) && pixel) {
    d.pixelShader = true;
    pixel->Release();
  }
  DWORD fvf = 0;
  dev->GetFVF(&fvf);
  {
    std::lock_guard<std::mutex> lock(g_declLock);
    if (fvf) {
      auto it = g_fvfDeclarations.find(fvf);
      if (it == g_fvfDeclarations.end())
        it = g_fvfDeclarations.emplace(fvf, makeDeclaration(elementsFromFvf(fvf))).first;
      declaration = it->second;
    } else {
      IDirect3DVertexDeclaration9 *object = nullptr;
      if (SUCCEEDED(dev->GetVertexDeclaration(&object)) && object) {
        auto it = g_declarations.find(object);
        if (it == g_declarations.end()) {
          D3DVERTEXELEMENT9 list[MAXD3DDECLLENGTH + 1];
          UINT count = 0;
          if (SUCCEEDED(object->GetDeclaration(list, &count)))
            it = g_declarations.emplace(object, makeDeclaration({list, list + count})).first;
        }
        if (it != g_declarations.end())
          declaration = it->second;
        object->Release();
      }
    }
  }
  if (declaration.elements.empty())
    return false;
  d.elements = declaration.elements.data();
  d.declarationHash = declaration.hash;
  d.device = dev;
  d.drawId = g_drawId++;
  d.occlusionQuery = g_openQueries > 0;
  d.backBufferWidth = g_backBufferWidth;
  d.backBufferHeight = g_backBufferHeight;

  IDirect3DSurface9 *target = nullptr;
  if (SUCCEEDED(dev->GetRenderTarget(0, &target)) && target) {
    D3DSURFACE_DESC desc;
    if (SUCCEEDED(target->GetDesc(&desc)))
      d.targetWidth = desc.Width, d.targetHeight = desc.Height, d.targetFormat = desc.Format;
    d.backBuffer = target == g_backBuffer;
    target->Release();
  } else {
    d.target = d.backBuffer = false;
  }

  static const D3DRENDERSTATETYPE states[] = {
      D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC,
      D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE,
      D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA, D3DRS_BLENDOPALPHA, D3DRS_COLORWRITEENABLE, D3DRS_CULLMODE,
      D3DRS_LIGHTING, D3DRS_COLORVERTEX, D3DRS_DIFFUSEMATERIALSOURCE, D3DRS_TEXTUREFACTOR, D3DRS_STENCILENABLE,
      D3DRS_STENCILFUNC, D3DRS_STENCILZFAIL, D3DRS_VERTEXBLEND, D3DRS_INDEXEDVERTEXBLENDENABLE, D3DRS_FOGENABLE,
      D3DRS_FOGCOLOR, D3DRS_FOGTABLEMODE, D3DRS_FOGVERTEXMODE, D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGDENSITY};
  for (D3DRENDERSTATETYPE state : states) {
    DWORD value = 0;
    dev->GetRenderState(state, &value);
    d.renderStates[state] = value;
  }
  // The stages up to the first disabled one (later ones are not used).
  static const D3DTEXTURESTAGESTATETYPE stageStates[] = {
      D3DTSS_COLOROP,   D3DTSS_COLORARG0, D3DTSS_COLORARG1, D3DTSS_COLORARG2,     D3DTSS_ALPHAOP,
      D3DTSS_ALPHAARG0, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2, D3DTSS_TEXCOORDINDEX, D3DTSS_RESULTARG};
  static const D3DSAMPLERSTATETYPE samplerStates[] = {D3DSAMP_MAGFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV};
  for (uint32_t i = 0; i < kStages; ++i) {
    Stage &stage = d.stages[i];
    for (D3DTEXTURESTAGESTATETYPE state : stageStates) {
      DWORD value = 0;
      dev->GetTextureStageState(i, state, &value);
      stage.states[state] = value;
    }
    for (D3DSAMPLERSTATETYPE state : samplerStates) {
      DWORD value = 0;
      dev->GetSamplerState(i, state, &value);
      stage.samplers[state] = value;
    }
    IDirect3DBaseTexture9 *texture = nullptr;
    if (SUCCEEDED(dev->GetTexture(i, &texture)) && texture) {
      stage.bound = true;
      stage.texture2D = texture->GetType() == D3DRTYPE_TEXTURE;
      stage.hash = textureHashOf(texture);
      texture->Release();
    }
    if (stage.states[D3DTSS_COLOROP] == D3DTOP_DISABLE)
      break;
  }
  dev->GetTransform(D3DTS_WORLD, &d.world);
  dev->GetTransform(D3DTS_VIEW, &d.view);
  dev->GetTransform(D3DTS_PROJECTION, &d.projection);
  dev->GetViewport(&d.viewport);
  dev->GetMaterial(&d.material);
  return true;
}

// The enabled lights go to the translator whenever the light state changed
// (dxvk-remix D3D9Rtx::emitDirtyLights), whatever the draw lights.
void collectLights(IDirect3DDevice9 *dev) {
  if (!g_lightsDirty.exchange(false))
    return;
  std::vector<D3DLIGHT9> enabled;
  {
    std::lock_guard<std::mutex> lock(g_lightLock);
    for (const auto &[index, light] : g_lights) {
      BOOL on = FALSE;
      if (SUCCEEDED(dev->GetLightEnable(index, &on)) && on)
        enabled.push_back(light);
    }
  }
  translator::lights(enabled);
}

// Binds the device's vertex and index buffers' shadows into the draw. The
// caller holds g_shadowLock.
void gatherBuffers(IDirect3DDevice9 *dev, Draw &d) {
  for (const D3DVERTEXELEMENT9 *e = d.elements; e->Stream != 0xff; ++e) {
    if (e->Stream >= kStreams || d.streams[e->Stream].data)
      continue;
    IDirect3DVertexBuffer9 *buffer = nullptr;
    UINT offset = 0, stride = 0;
    if (FAILED(dev->GetStreamSource(e->Stream, &buffer, &offset, &stride)) || !buffer)
      continue;
    auto it = g_shadows.find(buffer);
    if (it != g_shadows.end())
      d.streams[e->Stream] = {it->second.data.data(), uint32_t(it->second.data.size()), offset, stride, buffer,
                              it->second.version};
    buffer->Release();
  }
  if (d.indexed) {
    IDirect3DIndexBuffer9 *buffer = nullptr;
    if (SUCCEEDED(dev->GetIndices(&buffer)) && buffer) {
      auto it = g_shadows.find(buffer);
      if (it != g_shadows.end())
        d.indices = {it->second.data.data(), uint32_t(it->second.data.size()), it->second.is32, buffer,
                     it->second.version};
      buffer->Release();
    }
  }
}

// Captures the draw; returns whether the real device should rasterize it.
template <class Fill> bool capture(IDirect3DDevice9 *dev, Draw &d, Fill fillGeometry) {
  if (!g_started || !translator::active())
    return true;
  perf::Timer timer(perf::g_counters.capture);
  ++perf::g_counters.draws;
  Declaration declaration;
  ShaderState shaderState;
  collectLights(dev);
  if (!gatherState(dev, d, declaration, shaderState))
    return true;
  std::lock_guard<std::mutex> lock(g_shadowLock);
  fillGeometry(d);
  const bool pretransformed = findElement(d.elements, D3DDECLUSAGE_POSITIONT, 0) != nullptr;
  // A material texture without a hash: dxvk-remix leaves the draw.
  const DrawKind kind = selectTexture(d, g_env.config) ? classify(d, g_env.config, pretransformed) : DrawKind::Skip;
  const bool taken = translator::draw(d, kind);
  // What the translator did not take stays the real device's; occlusion
  // queries count what it rasterizes.
  return !taken || g_env.config.forwardDraws || g_openQueries > 0;
}

uint32_t verticesOf(D3DPRIMITIVETYPE type, UINT primitives) {
  switch (type) {
  case D3DPT_TRIANGLELIST: return primitives * 3;
  case D3DPT_TRIANGLESTRIP:
  case D3DPT_TRIANGLEFAN: return primitives + 2;
  case D3DPT_LINELIST: return primitives * 2;
  case D3DPT_LINESTRIP: return primitives + 1;
  default: return primitives;
  }
}

using DrawPrimitiveFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitiveFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT,
                                                            UINT, UINT);
using DrawPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *,
                                                       UINT);
using DrawIndexedPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT,
                                                              const void *, D3DFORMAT, const void *, UINT);

HRESULT STDMETHODCALLTYPE drawPrimitive(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE type, UINT start, UINT count) {
  Draw d;
  d.type = type;
  d.startVertex = start;
  d.primCount = count;
  if (!capture(dev, d, [&](Draw &draw) { gatherBuffers(dev, draw); }))
    return D3D_OK;
  auto fn = original<DrawPrimitiveFn>(dev, kDrawPrimitive);
  return fn ? fn(dev, type, start, count) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE drawIndexedPrimitive(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE type, INT baseVertex,
                                               UINT minIndex, UINT numVertices, UINT startIndex, UINT count) {
  Draw d;
  d.type = type;
  d.indexed = true;
  d.baseVertex = baseVertex;
  d.minIndex = minIndex;
  d.numVertices = numVertices;
  d.startIndex = startIndex;
  d.primCount = count;
  if (!capture(dev, d, [&](Draw &draw) { gatherBuffers(dev, draw); }))
    return D3D_OK;
  auto fn = original<DrawIndexedPrimitiveFn>(dev, kDrawIndexedPrimitive);
  return fn ? fn(dev, type, baseVertex, minIndex, numVertices, startIndex, count) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE drawPrimitiveUP(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE type, UINT count,
                                          const void *vertices, UINT stride) {
  Draw d;
  d.type = type;
  d.primCount = count;
  if (!capture(dev, d, [&](Draw &draw) {
        draw.streams[0] = {static_cast<const uint8_t *>(vertices), verticesOf(type, count) * stride, 0, stride};
      }))
    return D3D_OK;
  auto fn = original<DrawPrimitiveUPFn>(dev, kDrawPrimitiveUP);
  return fn ? fn(dev, type, count, vertices, stride) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE drawIndexedPrimitiveUP(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE type, UINT minIndex,
                                                 UINT numVertices, UINT count, const void *indices, D3DFORMAT format,
                                                 const void *vertices, UINT stride) {
  Draw d;
  d.type = type;
  d.indexed = true;
  d.minIndex = minIndex;
  d.numVertices = numVertices;
  d.primCount = count;
  if (!capture(dev, d, [&](Draw &draw) {
        const bool is32 = format == D3DFMT_INDEX32;
        draw.streams[0] = {static_cast<const uint8_t *>(vertices), (minIndex + numVertices) * stride, 0, stride};
        draw.indices = {static_cast<const uint8_t *>(indices), verticesOf(type, count) * (is32 ? 4u : 2u), is32};
      }))
    return D3D_OK;
  auto fn = original<DrawIndexedPrimitiveUPFn>(dev, kDrawIndexedPrimitiveUP);
  return fn ? fn(dev, type, minIndex, numVertices, count, indices, format, vertices, stride) : D3DERR_INVALIDCALL;
}

// --- Resource creation and uploads ------------------------------------------
using CreateTextureFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                     IDirect3DTexture9 **, HANDLE *);
using CreateVertexBufferFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, UINT, DWORD, DWORD, D3DPOOL,
                                                          IDirect3DVertexBuffer9 **, HANDLE *);
using CreateIndexBufferFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                         IDirect3DIndexBuffer9 **, HANDLE *);
using UpdateSurfaceFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *,
                                                     IDirect3DSurface9 *, const POINT *);
using UpdateTextureFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, IDirect3DBaseTexture9 *,
                                                     IDirect3DBaseTexture9 *);
using SetLightFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, DWORD, const D3DLIGHT9 *);
using CreateVertexDeclarationFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, const D3DVERTEXELEMENT9 *,
                                                               IDirect3DVertexDeclaration9 **);
using CreateQueryFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DQUERYTYPE, IDirect3DQuery9 **);
using QueryIssueFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DQuery9 *, DWORD);
using GetSwapChainFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, UINT, IDirect3DSwapChain9 **);
using CreateAdditionalSwapChainFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *,
                                                                 IDirect3DSwapChain9 **);

HRESULT STDMETHODCALLTYPE createTexture(IDirect3DDevice9 *dev, UINT width, UINT height, UINT levels, DWORD usage,
                                        D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *shared) {
  auto fn = original<CreateTextureFn>(dev, kCreateTexture);
  const HRESULT hr = fn ? fn(dev, width, height, levels, usage, format, pool, out, shared) : D3DERR_INVALIDCALL;
  if (SUCCEEDED(hr) && out && *out && !t_internal) {
    {
      std::lock_guard<std::mutex> lock(g_textureLock);
      g_textures.erase(*out); // an earlier texture at this address
    }
    hookTexture(*out);
  }
  return hr;
}

// The game's buffers on the real device, which processes vertices in
// hardware whatever the game asked for (behaviorFlags): software processing
// buffers are refused there.
DWORD bufferUsage(DWORD usage) { return t_internal ? usage : usage & ~DWORD(D3DUSAGE_SOFTWAREPROCESSING); }

void logBufferFailure(const char *kind, UINT length, DWORD usage, D3DPOOL pool, HRESULT hr) {
  static std::atomic<int> logged{0};
  if (FAILED(hr) && !t_internal && logged++ < 5)
    log("Create%sBuffer(%u bytes, usage 0x%lx, pool %d) failed: 0x%08lX", kind, length, usage, int(pool), hr);
}

HRESULT STDMETHODCALLTYPE createVertexBuffer(IDirect3DDevice9 *dev, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                                             IDirect3DVertexBuffer9 **out, HANDLE *shared) {
  auto fn = original<CreateVertexBufferFn>(dev, kCreateVertexBuffer);
  const HRESULT hr = fn ? fn(dev, length, bufferUsage(usage), fvf, pool, out, shared) : D3DERR_INVALIDCALL;
  logBufferFailure("Vertex", length, usage, pool, hr);
  if (SUCCEEDED(hr) && out && *out && !t_internal)
    trackBuffer(*out, length, false);
  return hr;
}

HRESULT STDMETHODCALLTYPE createIndexBuffer(IDirect3DDevice9 *dev, UINT length, DWORD usage, D3DFORMAT format,
                                            D3DPOOL pool, IDirect3DIndexBuffer9 **out, HANDLE *shared) {
  auto fn = original<CreateIndexBufferFn>(dev, kCreateIndexBuffer);
  const HRESULT hr = fn ? fn(dev, length, bufferUsage(usage), format, pool, out, shared) : D3DERR_INVALIDCALL;
  logBufferFailure("Index", length, usage, pool, hr);
  if (SUCCEEDED(hr) && out && *out && !t_internal)
    trackBuffer(*out, length, format == D3DFMT_INDEX32);
  return hr;
}

// A copy between textures carries the source's contents (D3DX and managed
// loaders fill a system-memory texture and copy it to the default pool):
// the destination's hash is the source's level 0's, with XXH3 (dxvk-remix
// hashes copies so whatever rtx.useObsoleteHashOnTextureUpload says).
void copyHash(IDirect3DBaseTexture9 *source, const void *destination) {
  {
    std::lock_guard<std::mutex> lock(g_textureLock);
    auto it = g_textures.find(destination);
    if (it != g_textures.end() && it->second.hash && !g_env.config.recomputeTextureHashOnWrite)
      return;
  }
  if (source->GetType() != D3DRTYPE_TEXTURE)
    return;
  const uint64_t hash = readLevelZero(static_cast<IDirect3DTexture9 *>(source), false);
  if (!hash)
    return;
  std::lock_guard<std::mutex> lock(g_textureLock);
  TextureState &t = g_textures[destination];
  t.hash = hash;
  t.written = false;
}

HRESULT STDMETHODCALLTYPE updateSurface(IDirect3DDevice9 *dev, IDirect3DSurface9 *source, const RECT *sourceRect,
                                        IDirect3DSurface9 *destination, const POINT *point) {
  if (source && destination && !sourceRect && !point) {
    IDirect3DTexture9 *from = levelZeroTexture(source), *to = levelZeroTexture(destination);
    if (from && to)
      copyHash(from, to);
  }
  auto fn = original<UpdateSurfaceFn>(dev, kUpdateSurface);
  return fn ? fn(dev, source, sourceRect, destination, point) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE updateTexture(IDirect3DDevice9 *dev, IDirect3DBaseTexture9 *source,
                                        IDirect3DBaseTexture9 *destination) {
  if (source && destination)
    copyHash(source, destination);
  auto fn = original<UpdateTextureFn>(dev, kUpdateTexture);
  return fn ? fn(dev, source, destination) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE setLight(IDirect3DDevice9 *dev, DWORD index, const D3DLIGHT9 *light) {
  if (light) {
    std::lock_guard<std::mutex> lock(g_lightLock);
    g_lights[index] = *light;
    g_lightsDirty = true;
  }
  auto fn = original<SetLightFn>(dev, kSetLight);
  return fn ? fn(dev, index, light) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE lightEnable(IDirect3DDevice9 *dev, DWORD index, BOOL enable) {
  using LightEnableFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, DWORD, BOOL);
  auto fn = original<LightEnableFn>(dev, kLightEnable);
  const HRESULT hr = fn ? fn(dev, index, enable) : D3DERR_INVALIDCALL;
  g_lightsDirty = true;
  return hr;
}

HRESULT STDMETHODCALLTYPE createVertexDeclaration(IDirect3DDevice9 *dev, const D3DVERTEXELEMENT9 *elements,
                                                  IDirect3DVertexDeclaration9 **out) {
  auto fn = original<CreateVertexDeclarationFn>(dev, kCreateVertexDeclaration);
  const HRESULT hr = fn ? fn(dev, elements, out) : D3DERR_INVALIDCALL;
  if (SUCCEEDED(hr) && out && *out) {
    std::lock_guard<std::mutex> lock(g_declLock);
    g_declarations.erase(*out); // an earlier declaration at this address
  }
  return hr;
}

using CreateVertexShaderFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, const DWORD *,
                                                          IDirect3DVertexShader9 **);

// Keeps a vertex shader's bytecode for vertex capture.
HRESULT STDMETHODCALLTYPE createVertexShader(IDirect3DDevice9 *dev, const DWORD *code, IDirect3DVertexShader9 **out) {
  auto fn = original<CreateVertexShaderFn>(dev, kCreateVertexShader);
  const HRESULT hr = fn ? fn(dev, code, out) : D3DERR_INVALIDCALL;
  if (SUCCEEDED(hr) && out && *out && !t_internal && g_env.config.useVertexCapture) {
    auto shader = std::make_shared<const ShaderCode>(parseShader(code));
    std::lock_guard<std::mutex> lock(g_shaderLock);
    g_shaders[*out] = std::move(shader); // also replaces an earlier shader at this address
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE queryIssue(IDirect3DQuery9 *query, DWORD flags) {
  if (query->GetType() == D3DQUERYTYPE_OCCLUSION) {
    if (flags & D3DISSUE_BEGIN)
      ++g_openQueries;
    else if ((flags & D3DISSUE_END) && g_openQueries > 0)
      --g_openQueries;
  }
  auto fn = original<QueryIssueFn>(query, kQueryIssue);
  return fn ? fn(query, flags) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE createQuery(IDirect3DDevice9 *dev, D3DQUERYTYPE type, IDirect3DQuery9 **out) {
  auto fn = original<CreateQueryFn>(dev, kCreateQuery);
  const HRESULT hr = fn ? fn(dev, type, out) : D3DERR_INVALIDCALL;
  if (SUCCEEDED(hr) && out && *out)
    hook(*out, kQueryIssue, reinterpret_cast<void *>(&queryIssue));
  return hr;
}

HRESULT STDMETHODCALLTYPE getSwapChain(IDirect3DDevice9 *dev, UINT index, IDirect3DSwapChain9 **out) {
  auto fn = original<GetSwapChainFn>(dev, kGetSwapChain);
  const HRESULT hr = fn ? fn(dev, index, out) : D3DERR_INVALIDCALL;
  if (SUCCEEDED(hr) && out && *out)
    hook(*out, kSwapChainPresent, reinterpret_cast<void *>(&swapChainPresent));
  return hr;
}

HRESULT STDMETHODCALLTYPE createAdditionalSwapChain(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp,
                                                    IDirect3DSwapChain9 **out) {
  auto fn = original<CreateAdditionalSwapChainFn>(dev, kCreateAdditionalSwapChain);
  const HRESULT hr = fn ? fn(dev, pp, out) : D3DERR_INVALIDCALL;
  if (SUCCEEDED(hr) && out && *out)
    hook(*out, kSwapChainPresent, reinterpret_cast<void *>(&swapChainPresent));
  return hr;
}

// --- Device creation and reset ----------------------------------------------
using ResetFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
using ResetExFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9Ex *, D3DPRESENT_PARAMETERS *, D3DDISPLAYMODEEX *);

void onReset(const D3DPRESENT_PARAMETERS &game) {
  if (g_started && g_fullscreen != !game.Windowed)
    translator::setFullscreen(!game.Windowed);
}

HRESULT STDMETHODCALLTYPE reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp) {
  auto fn = original<ResetFn>(dev, kReset);
  if (!fn || !pp)
    return D3DERR_INVALIDCALL;
  onReset(*pp);
  D3DPRESENT_PARAMETERS device = deviceParameters(g_gameWindow, *pp);
  const HRESULT hr = fn(dev, &device);
  returnParameters(device, *pp);
  if (SUCCEEDED(hr))
    refreshBackBuffer(dev);
  return hr;
}

HRESULT STDMETHODCALLTYPE resetEx(IDirect3DDevice9Ex *dev, D3DPRESENT_PARAMETERS *pp, D3DDISPLAYMODEEX *) {
  auto fn = original<ResetExFn>(dev, kResetEx);
  if (!fn || !pp)
    return D3DERR_INVALIDCALL;
  onReset(*pp);
  D3DPRESENT_PARAMETERS device = deviceParameters(g_gameWindow, *pp);
  const HRESULT hr = fn(dev, &device, nullptr);
  returnParameters(device, *pp);
  if (SUCCEEDED(hr))
    refreshBackBuffer(dev);
  return hr;
}

// The game's device is gone: OpenRemix stops while the game's threads are
// alive (its own would otherwise be torn down under it at exit).
ULONG STDMETHODCALLTYPE deviceRelease(IUnknown *self) {
  ReleaseFn fn = original<ReleaseFn>(self, kRelease);
  const ULONG references = fn ? fn(self) : 0;
  if (references == 0 && g_started && !t_internal) {
    translator::shutdown();
    g_started = false;
    g_startFailed = false;
    log("device released: OpenRemix shut down");
  }
  return references;
}

// The game asked for software (or mixed) vertex processing: the real device
// processes vertices in hardware all the same. Windows' software vertex
// processing runs vertex shaders on the CPU and slows a game drawn for RTX
// Remix (whose runtime ignores the request) to a crawl. The game is told
// what it asked for.
DWORD g_gameBehavior = 0;
bool g_softwareVertexProcessing = false;

HRESULT STDMETHODCALLTYPE getCreationParameters(IDirect3DDevice9 *dev, D3DDEVICE_CREATION_PARAMETERS *out) {
  using Fn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DDEVICE_CREATION_PARAMETERS *);
  auto fn = original<Fn>(dev, kGetCreationParameters);
  const HRESULT hr = fn ? fn(dev, out) : D3DERR_INVALIDCALL;
  if (SUCCEEDED(hr) && out && !t_internal) {
    out->BehaviorFlags = g_gameBehavior;
    out->hFocusWindow = g_gameWindow;
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE setSoftwareVertexProcessing(IDirect3DDevice9 *, BOOL software) {
  g_softwareVertexProcessing = software != FALSE;
  return D3D_OK;
}

BOOL STDMETHODCALLTYPE getSoftwareVertexProcessing(IDirect3DDevice9 *) {
  return g_softwareVertexProcessing || (g_gameBehavior & D3DCREATE_SOFTWARE_VERTEXPROCESSING);
}

void hookDevice(IDirect3DDevice9 *dev) {
  auto at = [&](unsigned slot, auto function) { hook(dev, slot, reinterpret_cast<void *>(function)); };
  at(kRelease, &deviceRelease);
  at(kGetCreationParameters, &getCreationParameters);
  at(kSetSoftwareVertexProcessing, &setSoftwareVertexProcessing);
  at(kGetSoftwareVertexProcessing, &getSoftwareVertexProcessing);
  at(kCreateAdditionalSwapChain, &createAdditionalSwapChain);
  at(kGetSwapChain, &getSwapChain);
  at(kReset, &reset);
  at(kPresent, &present);
  at(kCreateTexture, &createTexture);
  at(kCreateVertexBuffer, &createVertexBuffer);
  at(kCreateIndexBuffer, &createIndexBuffer);
  at(kUpdateSurface, &updateSurface);
  at(kUpdateTexture, &updateTexture);
  at(kSetLight, &setLight);
  at(kLightEnable, &lightEnable);
  at(kDrawPrimitive, &drawPrimitive);
  at(kDrawIndexedPrimitive, &drawIndexedPrimitive);
  at(kDrawPrimitiveUP, &drawPrimitiveUP);
  at(kDrawIndexedPrimitiveUP, &drawIndexedPrimitiveUP);
  at(kCreateVertexDeclaration, &createVertexDeclaration);
  at(kCreateQuery, &createQuery);
  at(kCreateVertexShader, &createVertexShader);
  IDirect3DDevice9Ex *ex = nullptr;
  if (SUCCEEDED(dev->QueryInterface(IID_IDirect3DDevice9Ex, reinterpret_cast<void **>(&ex))) && ex) {
    at(kPresentEx, &presentEx);
    at(kResetEx, &resetEx);
    ex->Release();
  }
  refreshBackBuffer(dev);
  input::hook();
}

using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
                                                    D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
using CreateDeviceExFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3D9Ex *, UINT, D3DDEVTYPE, HWND, DWORD,
                                                      D3DPRESENT_PARAMETERS *, D3DDISPLAYMODEEX *,
                                                      IDirect3DDevice9Ex **);

// Pure devices refuse the Get* calls the capture reads state with; vertices
// are processed in hardware (g_gameBehavior).
DWORD behaviorFlags(DWORD flags) {
  g_gameBehavior = flags;
  flags &= ~DWORD(D3DCREATE_PUREDEVICE);
  if (flags & (D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MIXED_VERTEXPROCESSING))
    flags = (flags & ~DWORD(D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MIXED_VERTEXPROCESSING)) |
            D3DCREATE_HARDWARE_VERTEXPROCESSING;
  return flags;
}

HRESULT STDMETHODCALLTYPE createDevice(IDirect3D9 *self, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                       D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out) {
  auto fn = original<CreateDeviceFn>(self, kCreateDevice);
  if (!fn || !pp)
    return D3DERR_INVALIDCALL;
  if (t_internal)
    return fn(self, adapter, type, focus, flags, pp, out);
  D3DPRESENT_PARAMETERS device = deviceParameters(focus, *pp);
  const HRESULT hr = fn(self, adapter, type, hiddenWindow(), behaviorFlags(flags), &device, out);
  returnParameters(device, *pp);
  log("CreateDevice(window %p, %ux%u%s, behavior 0x%lx): 0x%08lX", static_cast<void *>(g_gameWindow),
      g_backBufferWidth, g_backBufferHeight, g_fullscreen ? ", fullscreen" : "", flags, hr);
  if (SUCCEEDED(hr) && out && *out)
    hookDevice(*out);
  return hr;
}

HRESULT STDMETHODCALLTYPE createDeviceEx(IDirect3D9Ex *self, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                         D3DPRESENT_PARAMETERS *pp, D3DDISPLAYMODEEX *, IDirect3DDevice9Ex **out) {
  auto fn = original<CreateDeviceExFn>(self, kCreateDeviceEx);
  if (!fn || !pp)
    return D3DERR_INVALIDCALL;
  D3DPRESENT_PARAMETERS device = deviceParameters(focus, *pp);
  const HRESULT hr = fn(self, adapter, type, hiddenWindow(), behaviorFlags(flags), &device, nullptr, out);
  returnParameters(device, *pp);
  log("CreateDeviceEx(window %p, %ux%u%s, behavior 0x%lx): 0x%08lX", static_cast<void *>(g_gameWindow),
      g_backBufferWidth, g_backBufferHeight, g_fullscreen ? ", fullscreen" : "", flags, hr);
  if (SUCCEEDED(hr) && out && *out)
    hookDevice(*out);
  return hr;
}

} // namespace

InternalCalls::InternalCalls() : m_previous(t_internal) { t_internal = true; }
InternalCalls::~InternalCalls() { t_internal = m_previous; }

void initialize(const Environment &environment) {
  g_env = environment;
  renderer().prepare(environment.config, environment.pluginDirectory, environment.gameDirectory);
}

void hookDirect3D(IDirect3D9 *d3d) {
  if (!d3d)
    return;
  hook(d3d, kCreateDevice, reinterpret_cast<void *>(&createDevice));
  IDirect3D9Ex *ex = nullptr;
  if (SUCCEEDED(d3d->QueryInterface(IID_IDirect3D9Ex, reinterpret_cast<void **>(&ex))) && ex) {
    hook(d3d, kCreateDeviceEx, reinterpret_cast<void *>(&createDeviceEx));
    ex->Release();
  }
}

} // namespace openremix::d3d9::hooks
