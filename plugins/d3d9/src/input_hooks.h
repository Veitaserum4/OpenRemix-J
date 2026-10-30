#pragma once
#include <windows.h>

// While OpenRemix's menu is open the game must not see the mouse and the
// keyboard it polls: its user32 imports (GetCursorPos, SetCursorPos,
// GetRawInputData, GetRawInputBuffer, Get(Async)KeyState, GetKeyboardState)
// are patched in every module (Remix Plus's bridge, di_hook.cpp, does this
// with Detours). A game that recentres the cursor to mouse-look keeps
// recentring a position of its own while the real cursor moves over the
// menu, and sees no movement, buttons or keys.
namespace openremix::d3d9::input {

// Patches the modules loaded so far; again for modules loaded since.
void hook();

// The menu opened or closed (from Present).
void setMenuOpen(bool open);

} // namespace openremix::d3d9::input
