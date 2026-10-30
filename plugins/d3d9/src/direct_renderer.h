#pragma once
#include "renderer.h"

#include <memory>

// The renderer that calls remix.dll in this process: the 64-bit plugin's, and
// OpenRemixHost.exe's for 32-bit games.
namespace openremix::d3d9 {

class DirectRenderer : public Renderer {
public:
  DirectRenderer();
  ~DirectRenderer() override;
  bool start(HWND window, const Config &config, const std::filesystem::path &pluginDirectory,
             const std::filesystem::path &gameDirectory) override;
  void shutdown() override;
  void storeTexture(uint64_t hash, uint32_t format, uint32_t width, uint32_t height, const uint8_t *packed,
                    size_t size) override;
  void createMaterial(const MaterialDesc &material) override;
  void createMesh(uint64_t key, const Vertex *vertices, uint32_t vertexCount, const uint32_t *indices,
                  uint32_t indexCount, uint64_t material, const Skinning &skinning) override;
  void destroyMesh(uint64_t key) override;
  void createLight(uint64_t hash, const LightDesc &light) override;
  void destroyLight(uint64_t hash) override;
  void setCamera(const CameraDesc &camera) override;
  void drawInstance(const InstanceDesc &instance, const remixapi_Transform *bones) override;
  void drawLight(uint64_t hash) override;
  void setFog(const FogDesc &fog) override;
  void submitUi(uint32_t width, uint32_t height, const UiVertex *vertices, uint32_t vertexCount,
                const uint32_t *indices, uint32_t indexCount, const UiCommand *commands,
                uint32_t commandCount) override;
  void present(const char *screenshot) override;
  void setFullscreen(bool fullscreen) override;
  RendererStatus status() override;
  bool alive() override;

private:
  struct State;
  std::unique_ptr<State> m;
};

} // namespace openremix::d3d9
