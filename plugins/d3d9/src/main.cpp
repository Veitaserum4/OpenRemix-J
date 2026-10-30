// The plugin's d3d9.dll exports: Direct3DCreate9(Ex) hook the system D3D9's
// objects; everything else forwards to the system D3D9 as is.
#include "config.h"
#include "hooks.h"
#include "log.h"

#include <windows.h>

#include <d3d9.h>

#include <filesystem>
#include <mutex>
#include <string>

namespace {

using namespace openremix::d3d9;

HMODULE g_self = nullptr;
HMODULE g_system = nullptr;
std::once_flag g_once;

std::filesystem::path modulePath(HMODULE module) {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
    if (length < path.size()) {
      path.resize(length);
      return path;
    }
    path.resize(path.size() * 2);
  }
}

void initialize() {
  std::call_once(g_once, [] {
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    g_system = LoadLibraryW((std::wstring(system) + L"\\d3d9.dll").c_str());

    const std::filesystem::path plugin = modulePath(g_self).parent_path();
    const std::filesystem::path executable = modulePath(nullptr), game = executable.parent_path();

    hooks::Environment environment;
    environment.config = loadConfig(game, executable);
    environment.pluginDirectory = plugin;
    environment.gameDirectory = game;
    if (environment.config.log)
      openLog(game / "openremix-d3d9.log");
    log("OpenRemix D3D9 plugin (%s): %s, game %s", sizeof(void *) == 8 ? "64-bit" : "32-bit, OpenRemixHost.exe",
        plugin.string().c_str(), game.string().c_str());
    if (!g_system)
      log("cannot load the system d3d9.dll (error %lu)", GetLastError());
    hooks::initialize(environment);
  });
}

template <class Fn> Fn systemFunction(const char *name) {
  initialize();
  return g_system ? reinterpret_cast<Fn>(GetProcAddress(g_system, name)) : nullptr;
}

} // namespace

extern "C" {

IDirect3D9 *WINAPI PluginDirect3DCreate9(UINT version) {
  using CreateFn = IDirect3D9 *(WINAPI *)(UINT);
  auto create = systemFunction<CreateFn>("Direct3DCreate9");
  if (!create)
    return nullptr;
  IDirect3D9 *d3d = create(version);
  hooks::hookDirect3D(d3d);
  return d3d;
}

HRESULT WINAPI PluginDirect3DCreate9Ex(UINT version, IDirect3D9Ex **out) {
  using CreateExFn = HRESULT(WINAPI *)(UINT, IDirect3D9Ex **);
  auto create = systemFunction<CreateExFn>("Direct3DCreate9Ex");
  if (!create)
    return D3DERR_NOTAVAILABLE;
  const HRESULT hr = create(version, out);
  if (SUCCEEDED(hr) && out)
    hooks::hookDirect3D(*out);
  return hr;
}

int WINAPI PluginD3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR name) {
  auto fn = systemFunction<int(WINAPI *)(D3DCOLOR, LPCWSTR)>("D3DPERF_BeginEvent");
  return fn ? fn(color, name) : 0;
}
int WINAPI PluginD3DPERF_EndEvent() {
  auto fn = systemFunction<int(WINAPI *)()>("D3DPERF_EndEvent");
  return fn ? fn() : 0;
}
DWORD WINAPI PluginD3DPERF_GetStatus() {
  auto fn = systemFunction<DWORD(WINAPI *)()>("D3DPERF_GetStatus");
  return fn ? fn() : 0;
}
BOOL WINAPI PluginD3DPERF_QueryRepeatFrame() {
  auto fn = systemFunction<BOOL(WINAPI *)()>("D3DPERF_QueryRepeatFrame");
  return fn ? fn() : FALSE;
}
void WINAPI PluginD3DPERF_SetMarker(D3DCOLOR color, LPCWSTR name) {
  if (auto fn = systemFunction<void(WINAPI *)(D3DCOLOR, LPCWSTR)>("D3DPERF_SetMarker"))
    fn(color, name);
}
void WINAPI PluginD3DPERF_SetOptions(DWORD options) {
  if (auto fn = systemFunction<void(WINAPI *)(DWORD)>("D3DPERF_SetOptions"))
    fn(options);
}
void WINAPI PluginD3DPERF_SetRegion(D3DCOLOR color, LPCWSTR name) {
  if (auto fn = systemFunction<void(WINAPI *)(D3DCOLOR, LPCWSTR)>("D3DPERF_SetRegion"))
    fn(color, name);
}
void WINAPI PluginDirect3D9EnableMaximizedWindowedModeShim(UINT value) {
  if (auto fn = systemFunction<void(WINAPI *)(UINT)>("Direct3D9EnableMaximizedWindowedModeShim"))
    fn(value);
}
void *WINAPI PluginDirect3DShaderValidatorCreate9() {
  auto fn = systemFunction<void *(WINAPI *)()>("Direct3DShaderValidatorCreate9");
  return fn ? fn() : nullptr;
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_self = instance;
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}
