#pragma once
#include "scene/types.h"
#include <utility>
#include <vector>

namespace remix::scene {
  // std430 records shared with light_bvh.glsl. Analytic emitters need no
  // geometry.
  struct LightEmitter {
    float positionRadius[4] = {};
    float radiance[4] = {};
    float corners[3][4] = {};
    float directionCone[4] = {}, shaping[4] = {};
  };
  struct LightNode {
    float minimumPower[4] = {};
    float maximum[4] = {};  // xyz bounds; w largest emitter radius squared
    // Internal: left, right, first leaf rank, leaf count. Leaf: leaf rank,
    // unused, emitter, 1.
    uint32_t links[4] = {};
  };
  static_assert(sizeof(LightEmitter) == 112 && sizeof(LightNode) == 48);
  // A sphere light that moved, reshaped, changed power or switched on/off since
  // the previous update: its emitter now and before (zero radiance when absent).
  struct MovedLight {
    LightEmitter current, previous;
  };
  // The hierarchy has two parts. The static part holds the lights that were
  // present and at rest when it was built; its emitter and node indices stay
  // put until the next build (a light that leaves keeps its slot, unlit, and
  // returns to it). Lights that appear afterwards, and the emitters of
  // instances the host declared dynamic, form the dynamic part: a subtree of
  // its own behind the static nodes and emitters, rebuilt whenever its
  // membership changes. Only a build changes static indices, so only a build
  // invalidates what refers to them (visibility table, guiding history).
  // Without dynamic lights the hierarchy is the static part alone.
  class LightBvh {
  public:
    bool update(std::vector<SphereLight> lights);
    const std::vector<LightEmitter> &emitters() const { return m_emitters; }
    const std::vector<LightNode> &nodes() const { return m_nodes; }
    // The lights in emitter order: static slots (by type and identity, as
    // built; unlit when the light is gone), then the dynamic lights.
    const std::vector<SphereLight> &lights() const { return m_lights; }
    // Lights that changed in the last update (at most kMaxMoved; the denoiser's
    // local history confidence). Empty when nothing changed.
    static constexpr size_t kMaxMoved = 16;
    const std::vector<MovedLight> &moved() const { return m_moved; }
    // builds: the whole hierarchy (static indices changed). refits: updates
    // that kept them. dynamicBuilds: refits that rebuilt the dynamic part.
    uint64_t generation = 0, builds = 0, refits = 0, dynamicBuilds = 0;
    enum Reason : uint32_t { None, Initial, FirstDynamic, Settled, Retired, Cleared, ReasonCount };
    uint64_t buildReasons[ReasonCount] = {};
    uint32_t lastBuildReason = None;
    // Static slots in use, dynamic lights, static slots whose light is gone,
    // and lights of host-declared dynamic instances.
    uint32_t staticLights = 0, dynamicLights = 0, retiredSlots = 0, instanceLights = 0;
    // Emitters [0, staticEmitters()) and nodes [0, staticNodes()) belong to
    // the static part (with the root and the dynamic part's root).
    uint32_t staticEmitters() const { return m_staticCount; }
    uint32_t staticNodes() const { return m_staticNodes; }
    // Separate transport updates, scene-wide temporal response and the spatial
    // correspondence used by visibility guidance. Color animation changes only
    // transport unless its aggregate power change is significant.
    uint64_t lightingGeneration = 0, visibilityGeneration = 0;
    uint32_t active = 0, depth = 0;
    double updateMilliseconds = 0;
    // The last build (builds), and all of them.
    double buildMilliseconds = 0, buildTotalMilliseconds = 0;
    // Same bounded-distance importance estimate as the shader; useful for PDF
    // validation.
    static double weight(const LightNode &node, const float position[3], const float normal[3],
                         double range = 0);
    static double falloff(double distanceSquared, double range);
    // Updates a new light stays in the dynamic part before it counts as at
    // rest, and between builds.
    static constexpr uint64_t kSettle = 60, kBuildInterval = 120;
    // For a buffer that holds generation `since`: the emitter and node ranges
    // [first, end) written since then, ascending. False: upload everything (a
    // build, most of the tree changed, or `since` older than the
    // kept history).
    using Range = std::pair<uint32_t, uint32_t>;
    bool changesSince(uint64_t since, std::vector<Range> &emitters, std::vector<Range> &nodes) const;
    static constexpr size_t kWrittenHistory = 8;

  private:
    // What each of the last kWrittenHistory generations wrote.
    struct Written {
      uint64_t generation = 0;
      bool all = false;
      std::vector<Range> emitters, nodes;
    };
    std::vector<Written> m_written;
    std::vector<uint32_t> m_writtenEmitters, m_writtenNodes; // this update's writes
    bool m_writtenAll = false;
    void recordWritten();
    std::vector<MovedLight> m_moved;
    void build(std::vector<SphereLight> statics, std::vector<uint64_t> staticStamps,
               std::vector<SphereLight> dynamics, std::vector<uint64_t> dynamicStamps, Reason reason);
    void buildDynamic();
    void buildInto(uint32_t index, std::vector<uint32_t> &order, uint32_t begin, uint32_t end, uint32_t level);
    void bounds(uint32_t index);
    void significance(const std::vector<SphereLight> &sorted);
    uint64_t due(uint64_t oldest, uint64_t newest, uint32_t retired) const;
    // Input identity order survives radiance animation.
    std::vector<std::pair<uint64_t, uint32_t>> m_sourceKeys;
    std::vector<uint32_t> m_order, m_parents, m_leaves;
    std::vector<uint8_t> m_dirty;
    std::vector<SphereLight> m_sorted; // the last input, by type and identity
    std::vector<SphereLight> m_lights; // per emitter
    std::vector<SphereLight> m_temporalLights;
    std::vector<double> m_temporalEnergy; // per snapshot light: what significance() weighs it with
    std::vector<LightEmitter> m_emitters;
    std::vector<LightNode> m_nodes;
    // Per emitter: the update its geometry last changed in (or it appeared).
    std::vector<uint64_t> m_stamps;
    std::vector<uint8_t> m_present; // per static slot: its light is submitted
    bool m_split = false;           // root -> static part, dynamic part
    // Static and dynamic emitters; a part without lights keeps one unlit slot.
    uint32_t m_staticReal = 0, m_dynamicReal = 0;
    uint32_t m_staticCount = 0, m_staticNodes = 0, m_staticDepth = 0;
    uint64_t m_update = kSettle, m_builtAt = 0, m_due = ~0ull;
  };
} // namespace remix::scene
