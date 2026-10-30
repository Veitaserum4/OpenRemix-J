#pragma once
#include "renderer.h"

#include <atomic>
#include <cstdint>

// The plugin's bridge for 32-bit games: the renderer calls of the plugin in
// the game process (RemoteRenderer) go to OpenRemixHost.exe, a 64-bit process
// that owns remix.dll (DirectRenderer). Every command is fire-and-forget;
// the host answers through the shared status block, and paces the client
// with the frames semaphore. Both builds (x86 client, x64 host) share these
// layouts: plain fields only, sizes checked below.
namespace openremix::d3d9::bridge {

constexpr uint32_t kMagic = 0x4f524233;  // "ORB3"
constexpr uint32_t kVersion = 1;

enum Command : uint16_t {
  kStart = 1,       // StartPacket, then the game directory and executable (UTF-8, a newline between)
  kShutdown,
  kStoreTexture,    // TexturePacket, then the packed level
  kCreateMaterial,  // MaterialDesc
  kCreateMesh,      // MeshPacket, then vertices, indices, weights, bone indices
  kDestroyMesh,     // uint64 key
  kCreateLight,     // LightPacket
  kDestroyLight,    // uint64 hash
  kSetCamera,       // CameraDesc
  kDrawInstance,    // InstanceDesc, then boneCount transforms
  kDrawLight,       // uint64 hash
  kSetFog,          // FogDesc
  kSubmitUi,        // UiPacket, then vertices, indices, commands
  kPresent,         // the screenshot path (UTF-8, may be empty)
  kSetFullscreen,   // uint32
  kInput,           // InputPacket
};

// Every packet: this header, then `size` payload bytes.
struct PacketHeader {
  uint32_t magic;
  uint16_t command, reserved;
  uint32_t size;
  uint32_t sequence; // counts packets from 1: the host stops on a gap (desync)
};

struct StartPacket {
  uint64_t window; // HWND
  uint32_t framesAhead, reserved;
};

struct TexturePacket {
  uint64_t hash;
  uint32_t format, width, height, reserved;
};

struct MeshPacket {
  uint64_t key, material;
  uint32_t vertexCount, indexCount, bonesPerVertex, reserved;
};

struct LightPacket {
  uint64_t hash;
  LightDesc light;
};

struct UiPacket {
  uint32_t width, height, vertexCount, indexCount, commandCount, reserved;
};

struct InputPacket {
  uint32_t message, reserved;
  uint64_t w, l; // WPARAM, LPARAM (sign-extended)
};

// The shared block at the start of the bridge's memory; the ring follows.
struct SharedState {
  uint32_t magic, version;
  uint64_t ringSize;
  // Lock-free atomics: shared across processes, and 64-bit in the 32-bit client.
  alignas(64) std::atomic<uint64_t> written; // bytes the client has written (monotonic)
  alignas(64) std::atomic<uint64_t> read;    // bytes the host has consumed
  alignas(64) std::atomic<uint32_t> readerWaiting, writerWaiting;
  // The host's answers.
  alignas(64) std::atomic<uint32_t> hostState; // HostState
  RendererStatus status;
};
enum HostState : uint32_t { kHostStarting = 0, kHostRunning, kHostFailed, kHostStopped };

static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<uint32_t>::is_always_lock_free);
static_assert(sizeof(PacketHeader) == 16);
static_assert(sizeof(MaterialDesc) == 24 && sizeof(InstanceDesc) == 152 && sizeof(LightDesc) == 64);
static_assert(sizeof(CameraDesc) == 132 && sizeof(UiCommand) == 24 && sizeof(FogDesc) == 20);
static_assert(sizeof(Vertex) == 64 && sizeof(UiVertex) == 24 && sizeof(remixapi_Transform) == 48);
static_assert(sizeof(RendererStatus) == 296 && sizeof(LightPacket) == 72 && sizeof(InputPacket) == 24);

} // namespace openremix::d3d9::bridge
