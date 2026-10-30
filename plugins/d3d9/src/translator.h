#pragma once
#include "classify.h"
#include "config.h"
#include "draw.h"

#include <filesystem>
#include <vector>

// Turns classified draws into the renderer's meshes, materials, instances,
// lights, camera and UI list, and owns their lifetimes. Calls other than
// textureFilled come from the device's thread.
namespace openremix::d3d9::translator {

// Starts OpenRemix on `window` (the presenting thread).
bool start(HWND window, const Config &config, const std::filesystem::path &pluginDirectory,
           const std::filesystem::path &gameDirectory);
bool active();
void shutdown();

// Level 0 of a texture with this hash now holds `packed` (formats.h
// packLevel). Any thread, also before start.
void textureFilled(uint64_t hash, D3DFORMAT format, uint32_t width, uint32_t height,
                   const std::vector<uint8_t> &packed);

// A classified draw. True when the translator took it (World or Ui) into the
// frame; false leaves it to the real device.
bool draw(const Draw &draw, DrawKind kind);

// The device's enabled fixed-function lights, as the light state changed and
// at least once a frame (dxvk-remix emitDirtyLights): each is lit this frame.
void lights(const std::vector<D3DLIGHT9> &enabled);

// The frame is done: submits it and presents. `width` x `height` is the
// back buffer (the UI canvas).
void present(uint32_t width, uint32_t height);

// Fullscreen (exclusive) requested by the game's present parameters.
void setFullscreen(bool fullscreen);

} // namespace openremix::d3d9::translator
