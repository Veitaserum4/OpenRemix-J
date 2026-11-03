#include "scene/blackbody.h"
#include "scene/images.h"
#include "scene/light_bvh.h"
#include "scene/resources.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <remix/remix_native_ext.h>
#include <stdexcept>

void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main() {
  try {
    remix::NativeResources resources;
    remixapi_LightInfoUSDEXT usd{};
    usd.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_USD_EXT;
    for (int c = 0; c < 3; ++c)
      usd.transform.matrix[c][c] = 1;
    usd.transform.matrix[0][3] = 3;
    usd.transform.matrix[1][3] = 4;
    remixapi_Float3D color{1, .25f, 0};
    float intensity = 4, exposure = 2, radius = 2, width = 6, height = 4, length = 8, cone = .5f,
          softness = .3f, focus = 2;
    usd.pColor = &color;
    usd.pIntensity = &intensity;
    usd.pExposure = &exposure;
    usd.pRadius = &radius;
    usd.pWidth = &width;
    usd.pHeight = &height;
    usd.pLength = &length;
    remixapi_LightInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.hash = 11;
    info.pNext = &usd;
    remixapi_LightHandle handle{};
    for (auto type :
         {REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT, REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT,
          REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT, REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT}) {
      usd.lightType = type;
      resources.createLight(info, handle);
      resources.drawLight(handle);
      auto lights = resources.sphereLights();
      require(!lights.empty(), "USD area light conversion");
      for (const auto &light : lights) {
        require(light.radiance[0] == 16 && light.radiance[1] == 4 && light.radiance[2] == 0,
                "color intensity and exposure");
        require(light.radius > 0, "area dimensions");
      }
      resources.finishFrame();
    }
    usd.lightType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    usd.pConeAngleRadians = &cone;
    usd.pConeSoftness = &softness;
    usd.pFocus = &focus;
    resources.createLight(info, handle);
    resources.drawLight(handle);
    auto sphere = resources.sphereLights()[0];
    require(std::abs(sphere.coneCos - std::cos(cone)) < 1e-6f && sphere.direction[2] == -1 &&
                sphere.softness == softness && sphere.focus == focus,
            "spot shape and direction");
    resources.finishFrame();
    // Softness past 1 is valid, as in RTX Remix (HL2 RTX authors 2).
    float wideSoftness = 2;
    usd.pConeSoftness = &wideSoftness;
    resources.createLight(info, handle);
    resources.drawLight(handle);
    require(resources.sphereLights()[0].softness == wideSoftness, "cone softness past 1");
    resources.finishFrame();
    usd.pConeSoftness = &softness;
    usd.lightType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    resources.createLight(info, handle);
    resources.drawLight(handle);
    require(resources.light().isDistant && resources.light().direction[2] == -1 &&
                resources.light().radiance[0] == 16,
            "USD distant conversion");
    resources.finishFrame();
    remixapi_LightInfoDomeEXT dome{};
    dome.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
    for (int c = 0; c < 3; ++c)
      dome.transform.matrix[c][c] = 1;
    info.pNext = &dome;
    info.radiance = {2, 3, 4};
    resources.createLight(info, handle);
    resources.drawLight(handle);
    require(resources.dome().enabled && resources.dome().radiance[2] == 4, "dome radiance");
    resources.finishFrame();
    require(!resources.dome().enabled, "dome retirement");
    for (float kelvin : {1000.f, 3000.f, 6500.f, 10000.f}) {
      auto rgb = remix::scene::blackbody(kelvin);
      require(std::abs(.2126f * rgb[0] + .7152f * rgb[1] + .0722f * rgb[2] - 1) < 1e-4f,
              "temperature preserves luminance");
    }
    // A game light as RTX Remix splits it: an override of its intensity alone
    // keeps its color, not its radiance times the new intensity.
    remixapi_LightInfoSphereEXT gameSphere{};
    gameSphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    gameSphere.radius = 4;
    remixapi_LightInfo gameLight{};
    gameLight.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    gameLight.hash = 21;
    gameLight.pNext = &gameSphere;
    gameLight.radiance = {200, 100, 0};
    remixapi_LightHandle gameHandle{};
    resources.createLight(gameLight, gameHandle);
    remixapi_LightInfoUSDEXT gameOverride{};
    float gameIntensity = 3;
    gameOverride.pIntensity = &gameIntensity;
    auto overridden = resources.overrideLight(gameHandle, 22, gameOverride, false, nullptr);
    resources.drawLight(overridden);
    require(resources.sphereLights()[0].radiance[0] == 3 && resources.sphereLights()[0].radiance[1] == 1.5f,
            "game light intensity override");
    resources.finishFrame();
    gameOverride = {};
    remixapi_Float3D gameColor{0, 1, 0};
    gameOverride.pColor = &gameColor;
    overridden = resources.overrideLight(gameHandle, 22, gameOverride, false, nullptr);
    resources.drawLight(overridden);
    require(resources.sphereLights()[0].radiance[0] == 0 && resources.sphereLights()[0].radiance[1] == 200,
            "game light color override");
    resources.finishFrame();
    // The light tree picks no node that cannot emit toward the point, and only
    // those: the weight is 0 exactly where the light gives 0.
    remix::scene::SphereLight spot{};
    spot.id = 1;
    spot.radius = 1;
    spot.radiance[0] = spot.radiance[1] = spot.radiance[2] = 1;
    spot.direction[2] = -1;
    spot.coneCos = std::cos(30 * 3.14159265f / 180);
    const auto weightAt = [](const std::vector<remix::scene::SphereLight> &lights, float x, float y, float z) {
      remix::scene::LightBvh tree;
      tree.update(lights);
      const float length = std::sqrt(x * x + y * y + z * z);
      const float position[3] = {x, y, z}, normal[3] = {-x / length, -y / length, -z / length};
      return remix::scene::LightBvh::weight(tree.nodes()[0], position, normal);
    };
    const float rim = std::tan(29 * 3.14159265f / 180) * 100, past = std::tan(35 * 3.14159265f / 180) * 100;
    require(weightAt({spot}, 0, 0, -100) > 0, "a spot reaches along its axis");
    require(weightAt({spot}, rim, 0, -100) > 0, "a spot reaches inside its rim");
    require(weightAt({spot}, past, 0, -100) == 0, "a spot is not picked past its rim");
    require(weightAt({spot}, 0, 0, 100) == 0, "a spot is not picked behind it");
    require(weightAt({spot}, 2, 0, -.5f) > 0, "the light's size widens the bound up close");
    auto bare = spot;
    bare.coneCos = -1;
    require(weightAt({bare}, 0, 0, 100) > 0 && weightAt({bare}, past, 0, -100) > 0, "an unshaped light reaches all");
    // Two spots in one tree: picked toward either, and not away from both.
    auto second = spot;
    second.id = 2;
    second.direction[0] = 1, second.direction[2] = 0;
    second.position[0] = 4;
    const std::vector<remix::scene::SphereLight> both{spot, second};
    require(weightAt(both, 0, 0, -100) > 0 && weightAt(both, 100, 0, 0) > 0, "a node reaches where its lights do");
    require(weightAt(both, -100, 0, 100) == 0, "a node is not picked where none of its lights reach");
    // A one-sided triangle emits along its normal (+Z here) only.
    remix::scene::SphereLight triangle{};
    triangle.id = 3;
    triangle.triangle = 1;
    triangle.radius = 1;
    triangle.radiance[0] = 1;
    triangle.corners[1][0] = 1;
    triangle.corners[2][1] = 1;
    require(weightAt({triangle}, 0, 0, 100) > 0, "a triangle emits along its normal");
    require(weightAt({triangle}, 0, 0, -100) == 0, "a one-sided triangle does not emit behind it");
    triangle.triangle = 2;
    require(weightAt({triangle}, 0, 0, -100) > 0, "a two-sided triangle emits both ways");
    uint8_t block[16] = {255, 0};
    remixapi_TextureInfo texture{};
    texture.width = texture.height = 4;
    texture.depth = texture.mipLevels = 1;
    texture.data = block;
    texture.dataSize = 8;
    texture.format = REMIXAPI_FORMAT_BC4_UNORM_EXT;
    auto scalar = remix::scene::decodeTexture(texture);
    require(scalar.mips[0][0] == 255 && scalar.mips[0][1] == 255 && scalar.mips[0][3] == 255,
            "BC4 scalar channels");
    texture.dataSize = 16;
    texture.format = REMIXAPI_FORMAT_BC6H_UFLOAT_EXT;
    uint32_t rng = 123;
    bool hdr = false;
    for (unsigned i = 0; i < 256 && !hdr; ++i) {
      for (auto &b : block) {
        rng = rng * 1664525 + 1013904223;
        b = uint8_t(rng >> 24);
      }
      auto image = remix::scene::decodeTexture(texture);
      require(image.hdrMips.size() == 1 && image.hdrMips[0].size() == 64, "BC6H retains float pixels");
      hdr = *std::max_element(image.hdrMips[0].begin(), image.hdrMips[0].end()) > 1;
    }
    require(hdr, "BC6H preserves values above one");
    std::cout << "USD lights and HDR assets passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  } catch (...) {
    return 1;
  }
}
