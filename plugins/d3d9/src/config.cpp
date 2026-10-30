#include "config.h"

#include "profiles.h"

#include <remix/remix_c.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>

namespace openremix::d3d9 {
namespace {

// rtx.conf texture lists and the instance categories they put draws in (the
// public API bits; as OpenRemix's USD capture reads them).
constexpr std::pair<const char *, uint32_t> kCategoryLists[] = {
    {"rtx.worldSpaceUiTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_UI},
    {"rtx.worldSpaceUiBackgroundTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_MATTE},
    {"rtx.skyBoxTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_SKY},
    {"rtx.ignoreTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE},
    {"rtx.antiCulling.antiCullingTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_ANTI_CULLING},
    {"rtx.postfx.motionBlurMaskOutTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_MOTION_BLUR},
    {"rtx.opacityMicromapIgnoreTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_OPACITY_MICROMAP},
    {"rtx.hideInstanceTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN},
    {"rtx.particleTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE},
    {"rtx.beamTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_BEAM},
    {"rtx.decalTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC},
    {"rtx.dynamicDecalTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC},
    {"rtx.singleOffsetDecalTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_SINGLE_OFFSET},
    {"rtx.nonOffsetDecalTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_NO_OFFSET},
    {"rtx.cutoutTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_ALPHA_BLEND_TO_CUTOUT},
    {"rtx.terrainTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_TERRAIN},
    {"rtx.animatedWaterTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_ANIMATED_WATER},
    {"rtx.playerModelTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_MODEL},
    {"rtx.playerModelBodyTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_BODY},
    {"rtx.ignoreBakedLightingTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_BAKED_LIGHTING},
    {"rtx.ignoreAlphaOnTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_ALPHA_CHANNEL},
    {"rtx.ignoreTransparencyLayerTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_TRANSPARENCY_LAYER},
    {"rtx.particleEmitterTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE_EMITTER},
    {"rtx.smoothNormalsTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_SMOOTH_NORMALS},
    {"rtx.hairCardTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_HAIR_CARDS},
    {"rtx.legacyEmissiveTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_LEGACY_EMISSIVE},
};

std::string trim(const std::string &value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return value;
}

bool boolean(const std::string &value) {
  const std::string v = lower(value);
  return v == "true" || v == "1" || v == "yes" || v == "on";
}

std::vector<uint64_t> hashes(const std::string &value) {
  std::vector<uint64_t> out;
  std::stringstream list(value);
  for (std::string item; std::getline(list, item, ',');) {
    item = trim(item);
    if (item.empty())
      continue;
    char *end = nullptr;
    const uint64_t hash = std::strtoull(item.c_str(), &end, 16); // accepts the 0x prefix
    if (end && *end == '\0')
      out.push_back(hash);
  }
  return out;
}

std::string readFile(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return {};
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

std::filesystem::path environmentPath(const char *name) {
  const char *value = std::getenv(name);
  return value && *value ? std::filesystem::path(value) : std::filesystem::path();
}

} // namespace

void parseConfig(const std::string &text, Config &c) {
  std::stringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    if (const auto comment = line.find('#'); comment != std::string::npos)
      line.erase(comment);
    const auto equals = line.find('=');
    if (equals == std::string::npos)
      continue;
    const std::string key = trim(line.substr(0, equals)), value = trim(line.substr(equals + 1));
    if (key.empty())
      continue;

    bool list = false;
    for (const auto &[name, bit] : kCategoryLists) {
      if (key != name)
        continue;
      // A later file's list replaces an earlier one, as option layers do.
      for (auto it = c.categories.begin(); it != c.categories.end();)
        if ((it->second &= ~bit) == 0)
          it = c.categories.erase(it);
        else
          ++it;
      for (const uint64_t hash : hashes(value))
        c.categories[hash] |= bit;
      list = true;
    }
    if (list)
      continue;
    if (key == "rtx.uiTextures") {
      const auto values = hashes(value);
      c.uiTextures = {values.begin(), values.end()};
    } else if (key == "rtx.ignoreLights") {
      const auto values = hashes(value);
      c.ignoreLights = {values.begin(), values.end()};
    } else if (key == "rtx.lightmapTextures") {
      const auto values = hashes(value);
      c.lightmapTextures = {values.begin(), values.end()};
    } else if (key == "rtx.geometryAssetHashRuleString") {
      c.assetHashRule = parseHashRule(value);
    } else if (key == "rtx.useObsoleteHashOnTextureUpload") {
      c.useObsoleteHashOnTextureUpload = boolean(value);
    } else if (key == "rtx.recomputeTextureHashOnWrite") {
      c.recomputeTextureHashOnWrite = boolean(value);
    } else if (key == "rtx.useVertexCapture") {
      c.useVertexCapture = boolean(value);
    } else if (key == "rtx.useVertexCapturedNormals") {
      c.useVertexCapturedNormals = boolean(value);
    } else if (key == "rtx.useVertexCapturedTexcoords") {
      c.useVertexCapturedTexcoords = boolean(value);
    } else if (key == "rtx.useWorldMatricesForShaders") {
      c.useWorldMatricesForShaders = boolean(value);
    } else if (key == "rtx.allowCubemaps") {
      c.allowCubemaps = boolean(value);
    } else if (key == "rtx.drawCallRange") {
      // "first, last"
      char *end = nullptr;
      c.drawCallRangeFirst = uint32_t(std::strtoul(value.c_str(), &end, 10));
      while (end && *end && (*end == ',' || *end == ' '))
        ++end;
      c.drawCallRangeLast = end && *end ? uint32_t(std::strtoul(end, nullptr, 10)) : UINT32_MAX;
    } else if (key == "rtx.skyDrawcallIdThreshold") {
      c.skyDrawcallIdThreshold = uint32_t(std::strtoul(value.c_str(), nullptr, 10));
    } else if (key == "rtx.skyMinZThreshold") {
      c.skyMinZThreshold = std::strtof(value.c_str(), nullptr);
    } else if (key == "rtx.viewModel.enable") {
      c.viewModelEnable = boolean(value);
    } else if (key == "rtx.viewModel.maxZThreshold") {
      c.viewModelMaxZThreshold = std::strtof(value.c_str(), nullptr);
    } else if (key == "rtx.fusedWorldViewMode") {
      const std::string mode = lower(value);
      c.fusedWorldViewMode = mode == "view" || mode == "1" ? 1 : mode == "world" || mode == "2" ? 2 : 0;
    } else if (key == "rtx.orthographicIsUI") {
      c.orthographicIsUI = boolean(value);
    } else if (key == "rtx.preTransformedVerticesIsUI") {
      c.preTransformedVerticesIsUI = boolean(value);
    } else if (key == "rtx.enableAlphaBlend") {
      c.enableAlphaBlend = boolean(value);
    } else if (key == "rtx.enableAlphaTest") {
      c.enableAlphaTest = boolean(value);
    } else if (key == "rtx.zUp") {
      c.zUp = boolean(value);
    } else if (key == "rtx.leftHandedCoordinateSystem") {
      c.leftHanded = boolean(value);
    } else if (key == "rtx.sceneScale") {
      c.sceneScale = std::strtof(value.c_str(), nullptr);
    } else if (key == "openremix.backend") {
      c.backend = value;
    } else if (key == "openremix.forwardDraws") {
      c.forwardDraws = boolean(value);
    } else if (key == "openremix.mods") {
      c.mods = boolean(value);
    } else if (key == "openremix.log") {
      c.log = boolean(value);
    } else if (key == "openremix.bridgeMemory") {
      c.bridgeMemory = std::clamp(uint32_t(std::strtoul(value.c_str(), nullptr, 10)), 4u, 512u);
    } else if (key == "openremix.bridgeFramesAhead") {
      c.bridgeFramesAhead = std::clamp(uint32_t(std::strtoul(value.c_str(), nullptr, 10)), 1u, 3u);
    } else if (key == "openremix.screenshot") {
      c.screenshot = value;
    } else if (key == "openremix.screenshotFrame") {
      c.screenshotFrame = uint32_t(std::strtoul(value.c_str(), nullptr, 10));
    } else if (key == "openremix.hashLog") {
      c.hashLog = value;
    } else if (key == "openremix.screenshotAfterActive") {
      c.screenshotAfterActive = uint32_t(std::strtoul(value.c_str(), nullptr, 10));
    } else if (key.rfind("rtx.", 0) == 0) {
      c.passthrough.emplace_back(key, value);
    }
  }
}

uint32_t parseHashRule(const std::string &rule) {
  uint32_t bits = 0;
  std::stringstream list(lower(rule));
  for (std::string item; std::getline(list, item, ',');) {
    item = trim(item);
    if (item == "positions")
      bits |= kHashPositions;
    else if (item == "texcoords")
      bits |= kHashTexcoords;
    else if (item == "indices")
      bits |= kHashIndices;
    else if (item == "geometrydescriptor")
      bits |= kHashDescriptor;
    else if (item == "vertexlayout")
      bits |= kHashLayout;
  }
  return bits;
}

Config loadConfig(const std::filesystem::path &directory, const std::filesystem::path &executable) {
  Config config;
  std::filesystem::path dxvk = environmentPath("DXVK_CONFIG_FILE");
  if (dxvk.empty())
    dxvk = directory / "dxvk.conf";
  std::filesystem::path rtx = environmentPath("DXVK_RTX_CONFIG_FILE");
  if (rtx.empty())
    rtx = directory / "rtx.conf";
  parseConfig(readFile(dxvk), config);
  // The game's profile sits between dxvk.conf and the user's files, as
  // dxvk-remix layers its application defaults.
  const std::string path = executable.string();
  for (const Profile &profile : profiles())
    if (!path.empty() && std::regex_search(path, std::regex(profile.executable, std::regex::icase)))
      parseConfig(profile.settings, config);
  for (const auto &file : {directory / "user.conf", rtx, directory / "openremix.conf"})
    parseConfig(readFile(file), config);
  return config;
}

} // namespace openremix::d3d9
