#pragma once
#include "config.h"

#include <windows.h>

#include <d3d9.h>

#include <filesystem>

// Interception of the system D3D9: vtable entries of the objects it creates
// are replaced, so every draw, upload and present the game makes passes here.
// The real device renders offscreen draws, answers queries and readbacks,
// and never presents; OpenRemix presents into the game's window.
namespace openremix::d3d9::hooks {

struct Environment {
  Config config;
  std::filesystem::path pluginDirectory, gameDirectory;
};
void initialize(const Environment &environment);

// Hooks an IDirect3D9 / IDirect3D9Ex the system created.
void hookDirect3D(IDirect3D9 *d3d);

// While one lives on a thread, the hooks pass that thread's D3D9 calls
// through untouched: the plugin's own devices and resources (vertex capture's)
// share the game's patched vtables.
struct InternalCalls {
  InternalCalls();
  ~InternalCalls();
  InternalCalls(const InternalCalls &) = delete;
  InternalCalls &operator=(const InternalCalls &) = delete;

private:
  bool m_previous;
};

} // namespace openremix::d3d9::hooks
