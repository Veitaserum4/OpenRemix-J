#include "input_hooks.h"

#include "log.h"

#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <vector>

namespace openremix::d3d9::input {
namespace {

std::atomic<bool> g_menuOpen{false};

using GetCursorPosFn = BOOL(WINAPI *)(LPPOINT);
using SetCursorPosFn = BOOL(WINAPI *)(int, int);
using GetRawInputDataFn = UINT(WINAPI *)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using GetRawInputBufferFn = UINT(WINAPI *)(PRAWINPUT, PUINT, UINT);
using GetKeyStateFn = SHORT(WINAPI *)(int);
using GetKeyboardStateFn = BOOL(WINAPI *)(PBYTE);

GetCursorPosFn g_getCursorPos = nullptr;
SetCursorPosFn g_setCursorPos = nullptr;
GetRawInputDataFn g_getRawInputData = nullptr;
GetRawInputBufferFn g_getRawInputBuffer = nullptr;
GetKeyStateFn g_getAsyncKeyState = nullptr, g_getKeyState = nullptr;
GetKeyboardStateFn g_getKeyboardState = nullptr;

// The cursor position the game last saw or set, echoed while the menu is
// open (game thread).
POINT g_gameCursor = {};
bool g_gameCursorValid = false;

BOOL WINAPI hookedGetCursorPos(LPPOINT point) {
  if (!point) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  if (g_menuOpen.load(std::memory_order_relaxed) && g_gameCursorValid) {
    *point = g_gameCursor;
    return TRUE;
  }
  if (!g_getCursorPos(point))
    return FALSE;
  g_gameCursor = *point;
  g_gameCursorValid = true;
  return TRUE;
}

BOOL WINAPI hookedSetCursorPos(int x, int y) {
  g_gameCursor = {x, y};
  g_gameCursorValid = true;
  // The real cursor stays where the player moves it over the menu.
  if (g_menuOpen.load(std::memory_order_relaxed))
    return TRUE;
  return g_setCursorPos(x, y);
}

UINT WINAPI hookedGetRawInputData(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT header) {
  const UINT result = g_getRawInputData(input, command, data, size, header);
  if (!g_menuOpen.load(std::memory_order_relaxed) || command != RID_INPUT || !data || result == 0 ||
      result == UINT(-1))
    return result;
  RAWINPUT *raw = static_cast<RAWINPUT *>(data);
  if (raw->header.dwType == RIM_TYPEMOUSE && result >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE)) {
    if (!(raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
      raw->data.mouse.lLastX = raw->data.mouse.lLastY = 0;
    raw->data.mouse.usButtonFlags = raw->data.mouse.usButtonData = 0;
    raw->data.mouse.ulRawButtons = 0;
  } else if (raw->header.dwType == RIM_TYPEKEYBOARD &&
             result >= offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD)) {
    // A key-up of nothing: the game neither presses nor holds a key.
    raw->data.keyboard.VKey = 0xff;
    raw->data.keyboard.MakeCode = 0;
    raw->data.keyboard.Flags = RI_KEY_BREAK;
    raw->data.keyboard.Message = WM_KEYUP;
  }
  return result;
}

UINT WINAPI hookedGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT header) {
  if (!data || !g_menuOpen.load(std::memory_order_relaxed))
    return g_getRawInputBuffer(data, size, header);
  // Drained and dropped: nothing queued replays when the menu closes.
  UINT result = g_getRawInputBuffer(data, size, header);
  while (result != 0 && result != UINT(-1))
    result = g_getRawInputBuffer(data, size, header);
  return result == UINT(-1) ? result : 0;
}

SHORT WINAPI hookedGetAsyncKeyState(int key) {
  return g_menuOpen.load(std::memory_order_relaxed) ? 0 : g_getAsyncKeyState(key);
}

SHORT WINAPI hookedGetKeyState(int key) {
  return g_menuOpen.load(std::memory_order_relaxed) ? 0 : g_getKeyState(key);
}

BOOL WINAPI hookedGetKeyboardState(PBYTE state) {
  if (g_menuOpen.load(std::memory_order_relaxed) && state) {
    std::memset(state, 0, 256);
    return TRUE;
  }
  return g_getKeyboardState(state);
}

struct Patch {
  void *original;
  void *replacement;
};
std::vector<Patch> g_patches;
std::vector<HMODULE> g_patched;
std::mutex g_lock;

// Points `module`'s imports of the original functions at the replacements.
void patchModule(HMODULE module) {
  auto *base = reinterpret_cast<uint8_t *>(module);
  const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return;
  const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return;
  const IMAGE_DATA_DIRECTORY &imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!imports.VirtualAddress || !imports.Size)
    return;
  for (auto *d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(base + imports.VirtualAddress); d->Name; ++d) {
    for (auto *thunk = reinterpret_cast<IMAGE_THUNK_DATA *>(base + d->FirstThunk); thunk->u1.Function; ++thunk) {
      for (const Patch &p : g_patches) {
        if (reinterpret_cast<void *>(thunk->u1.Function) != p.original)
          continue;
        DWORD protection;
        if (VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), PAGE_READWRITE, &protection)) {
          thunk->u1.Function = reinterpret_cast<decltype(thunk->u1.Function)>(p.replacement);
          VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), protection, &protection);
        }
      }
    }
  }
}

} // namespace

void hook() {
  std::lock_guard<std::mutex> lock(g_lock);
  if (g_patches.empty()) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32)
      return;
    auto resolve = [&](auto &original, const char *name, auto *replacement) {
      original = reinterpret_cast<std::remove_reference_t<decltype(original)>>(GetProcAddress(user32, name));
      if (original)
        g_patches.push_back({reinterpret_cast<void *>(original), reinterpret_cast<void *>(replacement)});
    };
    resolve(g_getCursorPos, "GetCursorPos", &hookedGetCursorPos);
    resolve(g_setCursorPos, "SetCursorPos", &hookedSetCursorPos);
    resolve(g_getRawInputData, "GetRawInputData", &hookedGetRawInputData);
    resolve(g_getRawInputBuffer, "GetRawInputBuffer", &hookedGetRawInputBuffer);
    resolve(g_getAsyncKeyState, "GetAsyncKeyState", &hookedGetAsyncKeyState);
    resolve(g_getKeyState, "GetKeyState", &hookedGetKeyState);
    resolve(g_getKeyboardState, "GetKeyboardState", &hookedGetKeyboardState);
  }
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&hook), &self);
  std::vector<HMODULE> modules(1024);
  DWORD bytes = 0;
  if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(), DWORD(modules.size() * sizeof(HMODULE)), &bytes))
    return;
  modules.resize(std::min<size_t>(bytes / sizeof(HMODULE), modules.size()));
  size_t patched = 0;
  for (HMODULE module : modules) {
    if (module == self || std::find(g_patched.begin(), g_patched.end(), module) != g_patched.end())
      continue;
    patchModule(module);
    g_patched.push_back(module);
    ++patched;
  }
  if (patched)
    log("input: %zu modules' user32 imports patched", patched);
}

void setMenuOpen(bool open) {
  const bool was = g_menuOpen.exchange(open);
  // Closed: the game finds the cursor where it left it, so its next delta
  // is zero.
  if (was && !open && g_gameCursorValid && g_setCursorPos)
    g_setCursorPos(g_gameCursor.x, g_gameCursor.y);
}

} // namespace openremix::d3d9::input
