// OpenRemixHost.exe: runs OpenRemix (remix.dll is 64-bit) for the plugin in
// a 32-bit game. It reads the plugin's renderer calls from the bridge's
// channel and makes them on its own DirectRenderer, presenting into the
// game's window (OpenRemix's window in another process, platform/window.cpp).
// It exits with the game.
#include "bridge/channel.h"
#include "bridge/host_input.h"
#include "config.h"
#include "direct_renderer.h"
#include "log.h"
#include "perf.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace openremix::d3d9;
using namespace openremix::d3d9::bridge;

// Reads a payload's fields in order.
class Reader {
public:
  explicit Reader(const std::vector<uint8_t> &payload) : m_at(payload.data()), m_left(payload.size()) {}
  template <class T> T take() {
    T value = {};
    std::memcpy(&value, bytes(sizeof(T)), m_ok ? sizeof(T) : 0);
    return value;
  }
  // `size` bytes, or nullptr (and !ok()) past the end.
  const uint8_t *bytes(size_t size) {
    if (size > m_left) {
      m_ok = false;
      return nullptr;
    }
    const uint8_t *at = m_at;
    m_at += size;
    m_left -= size;
    return at;
  }
  std::string rest() {
    const size_t size = m_left;
    const uint8_t *at = bytes(size);
    return std::string(reinterpret_cast<const char *>(at), size);
  }
  bool ok() const { return m_ok; }

private:
  const uint8_t *m_at;
  size_t m_left;
  bool m_ok = true;
};

std::filesystem::path fromUtf8(const std::string &text) {
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
  std::wstring wide(size_t(size > 0 ? size : 0), L'\0');
  if (size > 0)
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), size);
  return wide;
}

void publish(Channel &channel, DirectRenderer &renderer) {
  const RendererStatus status = renderer.status();
  std::memcpy(const_cast<RendererStatus *>(&channel.state().status), &status, sizeof(status));
}

// Every few seconds: the host's frame rate and its time by command.
struct HostPerformance {
  int64_t since = perf::now(), waiting = 0;
  int64_t time[kInput + 1] = {};
  uint32_t count[kInput + 1] = {};
  uint32_t frames = 0;

  void report() {
    const int64_t now = perf::now();
    const double elapsed = perf::milliseconds(now - since);
    if (elapsed < 2000 || !frames)
      return;
    auto per = [&](Command c) { return perf::milliseconds(time[c]) / frames; };
    log("performance: %.1f fps; per frame: waiting for the game %.2f ms, present %.2f ms, meshes %.1f (%.2f ms), "
        "mesh destroys %.1f (%.2f ms), textures %.1f (%.2f ms), instances %.0f (%.2f ms), UI %.2f ms, input %.1f",
        frames * 1000 / elapsed, perf::milliseconds(waiting) / frames, per(kPresent),
        double(count[kCreateMesh]) / frames, per(kCreateMesh), double(count[kDestroyMesh]) / frames,
        per(kDestroyMesh), double(count[kStoreTexture]) / frames, per(kStoreTexture),
        double(count[kDrawInstance]) / frames, per(kDrawInstance), per(kSubmitUi), double(count[kInput]) / frames);
    *this = {};
  }
};

void fail(Channel &channel, const char *reason) {
  log("OpenRemixHost: %s", reason);
  RendererStatus &status = const_cast<RendererStatus &>(channel.state().status);
  std::strncpy(status.reason, reason, sizeof(status.reason) - 1);
  status.failed = 1;
  channel.state().hostState.store(kHostFailed, std::memory_order_release);
  channel.frameDone(); // a client waiting on the frame slot sees the failure
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc < 3)
    return 2;
  Channel channel;
  if (!channel.open(argv[1]))
    return 3;
  HANDLE client = OpenProcess(SYNCHRONIZE, FALSE, DWORD(_wtoi(argv[2])));
  if (!client) {
    fail(channel, "cannot watch the game's process");
    return 4;
  }
  channel.setPeer(client);

  // The plugin's folder holds openremix/ (this executable's folder).
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  const std::filesystem::path pluginDirectory = std::filesystem::path(self).parent_path().parent_path();

  DirectRenderer renderer;
  HostInput input;
  PacketHeader header;
  std::vector<uint8_t> payload;
  std::vector<remixapi_Transform> bones;
  bool running = true;
  HostPerformance performance;
  while (running) {
    // Messages for this thread's windows (OpenRemix's stand-in window).
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    const int64_t waitStart = perf::now();
    const Channel::Received received = channel.receive(header, payload, 50);
    performance.waiting += perf::now() - waitStart;
    if (received == Channel::Received::Timeout)
      continue;
    if (received == Channel::Received::Broken) {
      // The game is gone: nothing to shut down in order (OpenRemix's
      // shutdown can wait on a window that no longer exists).
      log("OpenRemixHost: %s", channel.error().c_str());
      TerminateProcess(GetCurrentProcess(), 0);
    }
    Reader in(payload);
    const int64_t commandStart = perf::now();
    switch (header.command) {
    case kStart: {
      const StartPacket start = in.take<StartPacket>();
      const std::string paths = in.rest();
      const size_t split = paths.find('\n');
      const std::filesystem::path game = fromUtf8(paths.substr(0, split));
      const std::filesystem::path executable =
          split == std::string::npos ? std::filesystem::path() : fromUtf8(paths.substr(split + 1));
      const Config config = loadConfig(game, executable);
      if (config.log)
        openLog(game / "openremix-host.log");
      log("OpenRemixHost: game %s, window %llx", game.string().c_str(), static_cast<unsigned long long>(start.window));
      if (renderer.start(reinterpret_cast<HWND>(intptr_t(start.window)), config, pluginDirectory, game)) {
        channel.state().hostState.store(kHostRunning, std::memory_order_release);
        publish(channel, renderer);
      } else {
        fail(channel, "OpenRemix did not start (see openremix-host.log)");
        running = false;
      }
      break;
    }
    case kShutdown:
      running = false;
      break;
    case kStoreTexture: {
      const TexturePacket texture = in.take<TexturePacket>();
      const size_t size = payload.size() - sizeof(texture);
      renderer.storeTexture(texture.hash, texture.format, texture.width, texture.height, in.bytes(size), size);
      break;
    }
    case kCreateMaterial:
      renderer.createMaterial(in.take<MaterialDesc>());
      break;
    case kCreateMesh: {
      const MeshPacket mesh = in.take<MeshPacket>();
      const size_t blend = size_t(mesh.vertexCount) * mesh.bonesPerVertex;
      const auto *vertices = reinterpret_cast<const Vertex *>(in.bytes(size_t(mesh.vertexCount) * sizeof(Vertex)));
      const auto *indices = reinterpret_cast<const uint32_t *>(in.bytes(size_t(mesh.indexCount) * 4));
      Skinning skinning;
      skinning.bonesPerVertex = mesh.bonesPerVertex;
      skinning.weights = reinterpret_cast<const float *>(in.bytes(blend * 4));
      skinning.indices = reinterpret_cast<const uint32_t *>(in.bytes(blend * 4));
      if (in.ok())
        renderer.createMesh(mesh.key, vertices, mesh.vertexCount, indices, mesh.indexCount, mesh.material, skinning);
      break;
    }
    case kDestroyMesh:
      renderer.destroyMesh(in.take<uint64_t>());
      break;
    case kCreateLight: {
      const LightPacket light = in.take<LightPacket>();
      renderer.createLight(light.hash, light.light);
      break;
    }
    case kDestroyLight:
      renderer.destroyLight(in.take<uint64_t>());
      break;
    case kSetCamera:
      renderer.setCamera(in.take<CameraDesc>());
      break;
    case kDrawInstance: {
      const InstanceDesc instance = in.take<InstanceDesc>();
      bones.resize(instance.boneCount);
      if (const uint8_t *data = in.bytes(bones.size() * sizeof(remixapi_Transform)))
        std::memcpy(bones.data(), data, bones.size() * sizeof(remixapi_Transform));
      if (in.ok())
        renderer.drawInstance(instance, bones.data());
      break;
    }
    case kDrawLight:
      renderer.drawLight(in.take<uint64_t>());
      break;
    case kSetFog:
      renderer.setFog(in.take<FogDesc>());
      break;
    case kSubmitUi: {
      const UiPacket ui = in.take<UiPacket>();
      const auto *vertices = reinterpret_cast<const UiVertex *>(in.bytes(size_t(ui.vertexCount) * sizeof(UiVertex)));
      const auto *indices = reinterpret_cast<const uint32_t *>(in.bytes(size_t(ui.indexCount) * 4));
      const auto *commands = reinterpret_cast<const UiCommand *>(in.bytes(size_t(ui.commandCount) * sizeof(UiCommand)));
      if (in.ok())
        renderer.submitUi(ui.width, ui.height, vertices, ui.vertexCount, indices, ui.indexCount, commands,
                          ui.commandCount);
      break;
    }
    case kPresent: {
      const std::string screenshot = in.rest();
      renderer.present(screenshot.c_str());
      publish(channel, renderer);
      channel.frameDone();
      break;
    }
    case kSetFullscreen:
      renderer.setFullscreen(in.take<uint32_t>() != 0);
      break;
    case kInput: {
      const InputPacket packet = in.take<InputPacket>();
      if (renderer.alive())
        input.deliver(packet.message, WPARAM(packet.w), LPARAM(int64_t(packet.l)));
      break;
    }
    default:
      log("OpenRemixHost: unknown command %u", unsigned(header.command));
      break;
    }
    if (header.command <= kInput) {
      performance.time[header.command] += perf::now() - commandStart;
      ++performance.count[header.command];
    }
    if (header.command == kPresent) {
      ++performance.frames;
      performance.report();
    }
    if (!in.ok())
      log("OpenRemixHost: command %u had a short payload (%u bytes)", unsigned(header.command), header.size);
  }
  // A shutdown that hangs does not keep the host alive.
  CreateThread(
      nullptr, 0,
      [](void *) -> DWORD {
        Sleep(10000);
        log("OpenRemixHost: shutdown did not finish in 10 s");
        TerminateProcess(GetCurrentProcess(), 1);
        return 0;
      },
      nullptr, 0, nullptr);
  renderer.shutdown();
  if (channel.state().hostState.load() != kHostFailed)
    channel.state().hostState.store(kHostStopped, std::memory_order_release);
  channel.frameDone();
  CloseHandle(client);
  return 0;
}
