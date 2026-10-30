// RemoteRenderer: the renderer of the 32-bit plugin. OpenRemix is 64-bit, so
// the calls go to OpenRemixHost.exe (bridge/host.cpp) through the bridge's
// channel; only Present waits, for a frame slot.
#include "bridge/channel.h"
#include "log.h"
#include "perf.h"
#include "renderer.h"

#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace openremix::d3d9 {
namespace {

using namespace bridge;

std::string utf8(const std::filesystem::path &path) {
  const std::wstring wide = path.wstring();
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(size > 0 ? size : 0), '\0');
  if (size > 0)
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), out.data(), size, nullptr, nullptr);
  return out;
}

bool keyboardMessage(UINT message) {
  return message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_CHAR || message == WM_SYSCHAR;
}
bool mouseMessage(UINT message) {
  return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || message == WM_MOUSEHWHEEL;
}
// What OpenRemix's window hears of the game's: input, focus and size.
// WM_INPUT never (it upsets Windows 11 when relayed; Remix Plus's bridge).
bool forwarded(UINT message) {
  return keyboardMessage(message) || message == WM_KEYUP || message == WM_SYSKEYUP || mouseMessage(message) ||
         message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ACTIVATE || message == WM_SIZE;
}

class RemoteRenderer;
RemoteRenderer *g_self = nullptr;

class RemoteRenderer : public Renderer {
public:
  void prepare(const Config &config, const std::filesystem::path &pluginDirectory,
               const std::filesystem::path &gameDirectory) override {
    m_config = config;
    m_plugin = pluginDirectory;
    m_game = gameDirectory;
  }

  bool start(HWND window, const Config &, const std::filesystem::path &,
             const std::filesystem::path &) override {
    if (!connect())
      return false;
    wchar_t executable[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    const std::string game = utf8(m_game) + "\n" + utf8(executable);
    StartPacket start = {uint64_t(intptr_t(window)), m_config.bridgeFramesAhead, 0};
    if (!send(kStart, {{&start, sizeof(start)}, {game.data(), game.size()}}))
      return false;
    m_window = window;
    g_self = this;
    m_previousProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&RemoteRenderer::windowProc)));
    m_started = true;
    log("OpenRemix bridge: OpenRemixHost.exe started for window %p", static_cast<void *>(window));
    return true;
  }

  void shutdown() override {
    if (m_window && m_previousProc &&
        GetWindowLongPtrW(m_window, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&RemoteRenderer::windowProc))
      SetWindowLongPtrW(m_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_previousProc));
    m_started = false;
    if (m_channel && !m_channel->broken()) {
      send(kShutdown, {});
      WaitForSingleObject(m_host, 5000);
    }
  }

  void storeTexture(uint64_t hash, uint32_t format, uint32_t width, uint32_t height, const uint8_t *packed,
                    size_t size) override {
    if (!connect())
      return;
    TexturePacket texture = {hash, format, width, height, 0};
    send(kStoreTexture, {{&texture, sizeof(texture)}, {packed, size}});
  }

  void createMaterial(const MaterialDesc &material) override { send(kCreateMaterial, {{&material, sizeof(material)}}); }

  void createMesh(uint64_t key, const Vertex *vertices, uint32_t vertexCount, const uint32_t *indices,
                  uint32_t indexCount, uint64_t material, const Skinning &skinning) override {
    MeshPacket mesh = {key, material, vertexCount, indexCount, skinning.bonesPerVertex, 0};
    const size_t blend = size_t(vertexCount) * skinning.bonesPerVertex;
    send(kCreateMesh, {{&mesh, sizeof(mesh)},
                       {vertices, size_t(vertexCount) * sizeof(Vertex)},
                       {indices, size_t(indexCount) * 4},
                       {skinning.weights, blend * 4},
                       {skinning.indices, blend * 4}});
  }

  void destroyMesh(uint64_t key) override { send(kDestroyMesh, {{&key, 8}}); }

  void createLight(uint64_t hash, const LightDesc &light) override {
    LightPacket packet = {hash, light};
    send(kCreateLight, {{&packet, sizeof(packet)}});
  }

  void destroyLight(uint64_t hash) override { send(kDestroyLight, {{&hash, 8}}); }
  void setCamera(const CameraDesc &camera) override { send(kSetCamera, {{&camera, sizeof(camera)}}); }

  void drawInstance(const InstanceDesc &instance, const remixapi_Transform *bones) override {
    send(kDrawInstance, {{&instance, sizeof(instance)}, {bones, size_t(instance.boneCount) * sizeof(*bones)}});
  }

  void drawLight(uint64_t hash) override { send(kDrawLight, {{&hash, 8}}); }
  void setFog(const FogDesc &fog) override { send(kSetFog, {{&fog, sizeof(fog)}}); }

  void submitUi(uint32_t width, uint32_t height, const UiVertex *vertices, uint32_t vertexCount,
                const uint32_t *indices, uint32_t indexCount, const UiCommand *commands,
                uint32_t commandCount) override {
    UiPacket ui = {width, height, vertexCount, indexCount, commandCount, 0};
    send(kSubmitUi, {{&ui, sizeof(ui)},
                     {vertices, size_t(vertexCount) * sizeof(UiVertex)},
                     {indices, size_t(indexCount) * 4},
                     {commands, size_t(commandCount) * sizeof(UiCommand)}});
  }

  void present(const char *screenshot) override {
    const size_t length = screenshot ? std::strlen(screenshot) : 0;
    if (send(kPresent, {{screenshot, length}}) && !m_channel->waitFrame())
      report();
  }

  void setFullscreen(bool fullscreen) override {
    const uint32_t value = fullscreen;
    send(kSetFullscreen, {{&value, 4}});
  }

  RendererStatus status() override {
    RendererStatus s = {};
    if (m_channel) {
      std::memcpy(&s, const_cast<const RendererStatus *>(&m_channel->state().status), sizeof(s));
      if (!alive())
        s.failed = 1;
    }
    return s;
  }

  bool alive() override {
    return m_channel && !m_channel->broken() &&
           m_channel->state().hostState.load(std::memory_order_acquire) < kHostFailed;
  }

private:
  // Creates the channel and starts the host, once.
  bool connect() {
    std::lock_guard<std::mutex> lock(m_connectLock);
    if (m_channel)
      return !m_channel->broken();
    m_channel = std::make_unique<Channel>();
    const std::wstring name = channelName(GetCurrentProcessId());
    if (!m_channel->create(name, uint64_t(m_config.bridgeMemory) << 20, m_config.bridgeFramesAhead)) {
      report();
      return false;
    }
    const std::filesystem::path host = m_plugin / "openremix" / "OpenRemixHost.exe";
    std::wstring command = L"\"" + host.wstring() + L"\" " + name + L" " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(host.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        host.parent_path().c_str(), &startup, &process)) {
      log("OpenRemix bridge: cannot start %s (error %lu)", utf8(host).c_str(), GetLastError());
      m_channel->state().hostState.store(kHostFailed);
      return false;
    }
    CloseHandle(process.hThread);
    m_host = process.hProcess;
    m_channel->setPeer(m_host);
    return true;
  }

  bool send(uint16_t command, std::initializer_list<Channel::Part> parts) {
    if (!m_channel || m_channel->broken())
      return false;
    perf::Timer timer(perf::g_counters.send);
    for (const Channel::Part &part : parts)
      perf::g_counters.sentBytes += part.size;
    if (m_channel->send(command, parts))
      return true;
    report();
    return false;
  }

  // Logs once why frames stopped reaching OpenRemix.
  void report() {
    if (m_reported)
      return;
    m_reported = true;
    const uint32_t state = m_channel->ready() ? m_channel->state().hostState.load() : uint32_t(kHostStarting);
    log("OpenRemix bridge stopped: %s%s%s", m_channel->error().c_str(),
        state == kHostFailed ? "; the host failed: " : "", state == kHostFailed ? m_channel->state().status.reason : "");
  }

  static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    RemoteRenderer *self = g_self;
    if (self && self->m_started && forwarded(message) && self->alive()) {
      InputPacket input = {message, 0, uint64_t(w), uint64_t(int64_t(l))};
      self->send(kInput, {{&input, sizeof(input)}});
      // OpenRemix's menu is open or has the keyboard or mouse: the game does
      // not see them (input_hooks.cpp hides what it polls).
      const RendererStatus &status = self->m_channel->state().status;
      if (((status.menuOpen || status.guiWantsKeyboard) && keyboardMessage(message)) ||
          ((status.menuOpen || status.guiWantsMouse) && mouseMessage(message)))
        return 0;
    }
    return CallWindowProcW(self ? self->m_previousProc : DefWindowProcW, window, message, w, l);
  }

  Config m_config;
  std::filesystem::path m_plugin, m_game;
  std::mutex m_connectLock;
  std::unique_ptr<Channel> m_channel;
  HANDLE m_host = nullptr;
  HWND m_window = nullptr;
  WNDPROC m_previousProc = nullptr;
  bool m_started = false, m_reported = false;
};

} // namespace

Renderer &renderer() {
  static RemoteRenderer instance;
  return instance;
}

} // namespace openremix::d3d9
