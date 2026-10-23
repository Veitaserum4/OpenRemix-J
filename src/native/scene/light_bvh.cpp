#include "scene/light_bvh.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <iterator>
#include <numeric>
#include <tuple>

namespace remix::scene {
  namespace {
    double powerScale(const SphereLight &light) {
      if (!light.triangle)
        return double(light.radius) * light.radius;
      double a[3], b[3];
      for (unsigned c = 0; c < 3; ++c) {
        a[c] = double(light.corners[1][c]) - light.corners[0][c];
        b[c] = double(light.corners[2][c]) - light.corners[0][c];
      }
      const double area =
          .5 * std::hypot(a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]);
      return area * light.triangle / (4 * 3.141592653589793);
    }
    LightEmitter emitterOf(const SphereLight &light) {
      LightEmitter e;
      std::copy_n(light.position, 3, e.positionRadius);
      e.positionRadius[3] = light.radius;
      std::copy_n(light.radiance, 3, e.radiance);
      e.radiance[3] = float(light.triangle);
      std::copy_n(light.direction, 3, e.directionCone);
      e.directionCone[3] = light.coneCos;
      e.shaping[0] = light.softness;
      e.shaping[1] = light.focus;
      for (unsigned v = 0; v < 3; ++v)
        std::copy_n(light.corners[v], 3, e.corners[v]);
      return e;
    }
  } // namespace
  namespace {
    using Key = std::pair<uint32_t, uint64_t>; // emitter order: type, identity
    Key keyOf(const SphereLight &light) { return {light.triangle, light.id}; }
    bool sameGeometry(const SphereLight &a, const SphereLight &b) {
      bool same = a.coneCos == b.coneCos && a.softness == b.softness && a.focus == b.focus &&
                  std::equal(a.direction, a.direction + 3, b.direction) && a.radius == b.radius &&
                  std::equal(a.position, a.position + 3, b.position);
      for (unsigned v = 0; v < 3; ++v)
        same &= std::equal(a.corners[v], a.corners[v] + 3, b.corners[v]);
      return same;
    }
    bool lit(const SphereLight &light) {
      return light.radiance[0] > 0 || light.radiance[1] > 0 || light.radiance[2] > 0;
    }
  } // namespace

  // The update a build is due in: once the lights waiting in the dynamic part
  // (not those of dynamic instances) have come to rest, or the static part is
  // mostly unlit slots; never sooner than kBuildInterval after the last one.
  uint64_t LightBvh::due(uint64_t oldest, uint64_t newest, uint32_t retired) const {
    if (oldest != ~0ull)
      return std::max(m_builtAt + kBuildInterval, std::min(newest + kSettle, oldest + 4 * kSettle));
    if (retired > 64 && retired > m_staticReal / 2)
      return m_builtAt + kBuildInterval;
    return ~0ull;
  }

  bool LightBvh::update(std::vector<SphereLight> lights, size_t stablePrefix, uint64_t prefixToken) {
    const auto start = std::chrono::steady_clock::now();
    m_writtenEmitters.clear();
    m_writtenNodes.clear();
    m_writtenAll = false;
    const uint64_t buildsBefore = builds;
    const auto finish = [&] {
      updateMilliseconds =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      if (builds != buildsBefore)
        buildTotalMilliseconds += buildMilliseconds = updateMilliseconds;
    };
    ++m_update;
    // A vouched leading run of inputs: the same lights, at the same input
    // positions, as in the last update.
    const size_t stable = prefixToken && prefixToken == m_prefixToken && stablePrefix == m_prefixLength &&
                                  stablePrefix <= lights.size() && stablePrefix <= m_sourceKeys.size()
                              ? stablePrefix
                              : 0;
    m_prefixToken = prefixToken;
    m_prefixLength = prefixToken ? stablePrefix : 0;
    // The order by type and identity (the input index breaks ties, so it is
    // the order a full sort makes). The leading run of inputs whose keys are
    // the last update's keeps its sorted order; only the rest is sorted and
    // merged in. Hosts that submit a large stable set first (the persistent
    // static set's emitters) pay for the few lights that come and go.
    const auto less = [&](uint32_t a, uint32_t b) {
      return std::tie(lights[a].triangle, lights[a].id, a) < std::tie(lights[b].triangle, lights[b].id, b);
    };
    size_t same = stable;
    for (const size_t common = std::min(lights.size(), m_sourceKeys.size());
         same < common && m_sourceKeys[same] == std::pair{lights[same].id, lights[same].triangle};)
      ++same;
    const bool reordered = same != lights.size() || same != m_sourceKeys.size();
    if (reordered) {
      m_sourceKeys.resize(same);
      for (size_t i = same; i < lights.size(); ++i)
        m_sourceKeys.emplace_back(lights[i].id, lights[i].triangle);
      std::vector<uint32_t> kept, added(lights.size() - same);
      kept.reserve(same);
      for (const uint32_t i : m_order)
        if (i < same)
          kept.push_back(i);
      std::iota(added.begin(), added.end(), uint32_t(same));
      std::sort(added.begin(), added.end(), less);
      m_order.clear();
      m_order.reserve(lights.size());
      std::merge(kept.begin(), kept.end(), added.begin(), added.end(), std::back_inserter(m_order), less);
    }
    std::vector<SphereLight> ordered;
    ordered.reserve(lights.size());
    for (auto i : m_order)
      ordered.push_back(lights[i]);
    lights = std::move(ordered);
    // Sphere lights that changed since the last update, now and before (both
    // lists are sorted with sphere lights first, by identity).
    m_moved.clear();
    {
      auto zero = [](LightEmitter e) {
        std::fill_n(e.radiance, 3, 0.f);
        return e;
      };
      size_t a = 0, b = 0;
      while (m_moved.size() < kMaxMoved) {
        const bool hasA = a < lights.size() && lights[a].triangle == 0;
        const bool hasB = b < m_sorted.size() && m_sorted[b].triangle == 0;
        if (!hasA && !hasB)
          break;
        if (hasA && hasB && lights[a].id == m_sorted[b].id) {
          if (!(lights[a] == m_sorted[b]))
            m_moved.push_back({emitterOf(lights[a]), emitterOf(m_sorted[b])});
          ++a, ++b;
        } else if (hasA && (!hasB || lights[a].id < m_sorted[b].id)) {
          const auto e = emitterOf(lights[a++]); // switched on
          m_moved.push_back({e, zero(e)});
        } else {
          const auto e = emitterOf(m_sorted[b++]); // switched off
          m_moved.push_back({zero(e), e});
        }
      }
    }
    // With the order kept, a vouched light is where it was and as it was.
    bool unchanged;
    if (stable && !reordered && lights.size() == m_sorted.size()) {
      unchanged = true;
      for (size_t j = 0; unchanged && j < lights.size(); ++j)
        unchanged = m_order[j] < stable || lights[j] == m_sorted[j];
    } else {
      unchanged = lights == m_sorted;
    }
    if (unchanged && m_update < m_due) {
      finish();
      return false;
    }
    if (lights.empty()) {
      m_nodes.clear();
      m_parents.clear();
      m_leaves.clear();
      m_emitters.clear();
      m_lights.clear();
      m_stamps.clear();
      m_present.clear();
      m_split = false;
      m_staticReal = m_dynamicReal = m_staticCount = m_staticNodes = m_staticDepth = 0;
      active = depth = staticLights = dynamicLights = retiredSlots = instanceLights = 0;
      m_due = ~0ull;
      m_builtAt = m_update;
      ++builds;
      ++buildReasons[lastBuildReason = Cleared];
      ++visibilityGeneration;
      significance(lights);
      m_sorted.clear();
      ++generation;
      m_writtenAll = true;
      recordWritten();
      finish();
      return true;
    }
    Reason reason = m_nodes.empty() ? Initial : None;
    bool geometryChanged = false;
    // Lights without a static slot, and the update their geometry last changed in.
    std::vector<SphereLight> extra;
    std::vector<uint64_t> extraStamps;
    uint64_t oldest = ~0ull, newest = 0;
    uint32_t retired = 0;
    if (reason == None) {
      m_dirty.assign(m_nodes.size(), 0);
      // Static slots are in emitter order, as the input: one pass matches them.
      size_t in = 0;
      for (uint32_t slot = 0; slot < m_staticReal; ++slot) {
        auto &held = m_lights[slot];
        const auto key = keyOf(held);
        while (in < lights.size() && keyOf(lights[in]) < key)
          extra.push_back(lights[in++]);
        if (in < lights.size() && keyOf(lights[in]) == key) {
          // A vouched light that held its slot last update still holds it unchanged.
          const bool vouched = m_order[in] < stable && m_present[slot];
          const auto &light = lights[in++];
          if (!vouched && (!m_present[slot] || !(light == held))) {
            if (!sameGeometry(light, held)) {
              m_stamps[slot] = m_update;
              geometryChanged = true;
            }
            held = light;
            m_emitters[slot] = emitterOf(light);
            m_writtenEmitters.push_back(slot);
            m_dirty[m_leaves[slot]] = 1;
            m_present[slot] = 1;
          }
        } else {
          // Gone: the slot stays, unlit, for the light's return.
          if (m_present[slot]) {
            m_present[slot] = 0;
            std::fill_n(held.radiance, 3, 0.f);
            m_emitters[slot] = emitterOf(held);
            m_writtenEmitters.push_back(slot);
            m_dirty[m_leaves[slot]] = 1;
          }
          ++retired;
        }
      }
      extra.insert(extra.end(), lights.begin() + in, lights.end());
      const SphereLight *held = m_lights.data() + m_staticCount;
      extraStamps.resize(extra.size());
      bool sameKeys = extra.size() == m_dynamicReal;
      for (size_t i = 0, d = 0; i < extra.size(); ++i) {
        const auto key = keyOf(extra[i]);
        while (d < m_dynamicReal && keyOf(held[d]) < key)
          ++d;
        if (d < m_dynamicReal && keyOf(held[d]) == key) {
          extraStamps[i] = sameGeometry(extra[i], held[d]) ? m_stamps[m_staticCount + d] : m_update;
          sameKeys &= d == i;
          ++d;
        } else {
          extraStamps[i] = m_update;
          sameKeys = false;
        }
        if (!extra[i].dynamic) {
          oldest = std::min(oldest, extraStamps[i]);
          newest = std::max(newest, extraStamps[i]);
        }
      }
      if (!extra.empty() && !m_split)
        reason = FirstDynamic; // the hierarchy gains its dynamic part
      else if (m_update >= due(oldest, newest, retired))
        reason = oldest != ~0ull ? Settled : Retired;
      if (reason == None && unchanged) {
        m_due = due(oldest, newest, retired);
        finish();
        return false;
      }
      if (reason == None) {
        if (sameKeys) {
          for (size_t i = 0; i < extra.size(); ++i) {
            const size_t slot = m_staticCount + i;
            if (extra[i] == m_lights[slot])
              continue;
            geometryChanged |= extraStamps[i] != m_stamps[slot];
            m_lights[slot] = extra[i];
            m_stamps[slot] = extraStamps[i];
            m_emitters[slot] = emitterOf(extra[i]);
            m_writtenEmitters.push_back(uint32_t(slot));
            m_dirty[m_leaves[slot]] = 1;
          }
        } else {
          m_lights.resize(m_staticCount);
          m_stamps.resize(m_staticCount);
          m_dynamicReal = uint32_t(extra.size());
          m_lights.insert(m_lights.end(), extra.begin(), extra.end());
          m_stamps.insert(m_stamps.end(), extraStamps.begin(), extraStamps.end());
          buildDynamic();
          ++dynamicBuilds;
          geometryChanged = true;
        }
        for (size_t i = m_nodes.size(); i-- > 0;)
          if (m_dirty[i]) {
            bounds(uint32_t(i));
            if (i)
              m_dirty[m_parents[i]] = 1;
          }
        ++refits;
      }
    }
    if (reason != None) {
      // Lights at rest go to the static part; those of dynamic instances and
      // those that appeared or moved lately form the dynamic part.
      std::vector<std::pair<SphereLight, uint64_t>> statics, dynamics;
      const auto place = [&](const SphereLight &light, uint64_t stamp) {
        (!light.dynamic && m_update - stamp >= kSettle ? statics : dynamics).emplace_back(light, stamp);
      };
      if (reason == Initial) {
        for (const auto &light : lights)
          place(light, light.dynamic ? m_update : m_update - kSettle);
      } else {
        for (uint32_t slot = 0; slot < m_staticReal; ++slot)
          if (m_present[slot])
            place(m_lights[slot], m_stamps[slot]);
        for (size_t i = 0; i < extra.size(); ++i)
          place(extra[i], extraStamps[i]);
        const auto byKey = [](const auto &a, const auto &b) { return keyOf(a.first) < keyOf(b.first); };
        std::stable_sort(statics.begin(), statics.end(), byKey);
        std::stable_sort(dynamics.begin(), dynamics.end(), byKey);
      }
      std::vector<SphereLight> parts[2];
      std::vector<uint64_t> stamps[2];
      for (unsigned part = 0; part < 2; ++part)
        for (auto &[light, stamp] : part ? dynamics : statics) {
          parts[part].push_back(light);
          stamps[part].push_back(stamp);
        }
      build(std::move(parts[0]), std::move(stamps[0]), std::move(parts[1]), std::move(stamps[1]), reason);
      geometryChanged = true;
      retired = 0;
      oldest = ~0ull, newest = 0;
      for (uint32_t i = 0; i < m_dynamicReal; ++i)
        if (!m_lights[m_staticCount + i].dynamic) {
          oldest = std::min(oldest, m_stamps[m_staticCount + i]);
          newest = std::max(newest, m_stamps[m_staticCount + i]);
        }
    }
    if (geometryChanged)
      ++visibilityGeneration;
    m_due = due(oldest, newest, retired);
    active = instanceLights = 0;
    for (const auto &light : m_lights)
      active += lit(light);
    for (const auto &light : lights)
      instanceLights += light.dynamic;
    retiredSlots = retired;
    staticLights = m_staticReal - retired;
    dynamicLights = m_dynamicReal;
    significance(lights);
    m_sorted = std::move(lights);
    ++generation;
    recordWritten();
    finish();
    return true;
  }

  void LightBvh::recordWritten() {
    const auto ranges = [](std::vector<uint32_t> &written) {
      std::sort(written.begin(), written.end());
      std::vector<Range> out;
      for (const uint32_t i : written) {
        if (!out.empty() && i <= out.back().second) {
          out.back().second = std::max(out.back().second, i + 1);
          continue;
        }
        out.emplace_back(i, i + 1);
      }
      return out;
    };
    Written written;
    written.generation = generation;
    // A resize rewrites only the dynamic part, whose writes are listed; the
    // buffers' new sizes are the caller's to check.
    written.all = m_writtenAll;
    if (!written.all) {
      written.emitters = ranges(m_writtenEmitters);
      written.nodes = ranges(m_writtenNodes);
    }
    if (m_written.size() == kWrittenHistory)
      m_written.erase(m_written.begin());
    m_written.push_back(std::move(written));
    m_writtenEmitters.clear();
    m_writtenNodes.clear();
  }

  bool LightBvh::changesSince(uint64_t since, std::vector<Range> &emitters, std::vector<Range> &nodes) const {
    emitters.clear();
    nodes.clear();
    if (since == generation)
      return true;
    size_t first = 0;
    while (first < m_written.size() && m_written[first].generation <= since)
      ++first;
    // The history must hold every generation after `since`.
    if (first == m_written.size() || m_written[first].generation != since + 1)
      return false;
    const auto merge = [](std::vector<Range> &out) {
      std::sort(out.begin(), out.end());
      size_t kept = 0;
      for (size_t i = 0; i < out.size(); ++i) {
        if (kept && out[i].first <= out[kept - 1].second)
          out[kept - 1].second = std::max(out[kept - 1].second, out[i].second);
        else
          out[kept++] = out[i];
      }
      out.resize(kept);
    };
    for (size_t i = first; i < m_written.size(); ++i) {
      const auto &written = m_written[i];
      if (written.all)
        return false;
      emitters.insert(emitters.end(), written.emitters.begin(), written.emitters.end());
      nodes.insert(nodes.end(), written.nodes.begin(), written.nodes.end());
    }
    merge(emitters);
    merge(nodes);
    // Ranges written by earlier generations may lie past the current end.
    const auto clamp = [](std::vector<Range> &out, size_t size) {
      size_t kept = 0;
      for (auto range : out) {
        range.second = std::min(range.second, uint32_t(size));
        if (range.first < range.second)
          out[kept++] = range;
      }
      out.resize(kept);
    };
    clamp(emitters, m_emitters.size());
    clamp(nodes, m_nodes.size());
    // Scattered changes over most of the tree: one copy of everything is cheaper.
    size_t emitterCount = 0, nodeCount = 0;
    for (const auto &range : emitters)
      emitterCount += range.second - range.first;
    for (const auto &range : nodes)
      nodeCount += range.second - range.first;
    return emitterCount * 2 <= m_emitters.size() && nodeCount * 2 <= m_nodes.size() && emitters.size() + nodes.size() <= 4096;
  }

  // Tiny animated-emission fluctuations still update the GPU light data, but
  // must not keep every local-light filter/cache permanently in fast response.
  // Compare against the last significant snapshot, not the preceding frame,
  // so gradual ramps eventually cross the threshold too. A light that moved,
  // reshaped, appeared or went counts with its whole power: one animated or
  // carried light among many (a blinking siren, a bot's flashlight) must not
  // reset the local denoiser every frame, while a dominant one still does.
  // This is a scene-wide fast-response switch, not a per-emitter filter.
  void LightBvh::significance(const std::vector<SphereLight> &sorted) {
    const auto &before = m_temporalLights;
    double changed = 0, totalOld = 0, totalNew = 0;
    const auto energy = [](const SphereLight &light) {
      return (double(light.radiance[0]) + light.radiance[1] + light.radiance[2]) * powerScale(light);
    };
    // The snapshot keeps its lights' energies, and a light that is the same
    // as in the snapshot (most of them, in a large static set) reuses its
    // own: triangle areas are not measured again every update.
    std::vector<double> energies(sorted.size());
    size_t a = 0, b = 0;
    while (a < sorted.size() || b < before.size()) {
      if (a < sorted.size() && b < before.size() && keyOf(sorted[a]) == keyOf(before[b])) {
        const auto &now = sorted[a], &then = before[b];
        const double oldEnergy = m_temporalEnergy[b];
        if (now == then) {
          energies[a++] = oldEnergy;
          ++b;
          totalOld += oldEnergy;
          totalNew += oldEnergy;
          continue;
        }
        const double newEnergy = energies[a++] = energy(now);
        ++b;
        double difference = 0;
        for (unsigned c = 0; c < 3; ++c)
          difference += std::abs(double(now.radiance[c]) - then.radiance[c]);
        const bool moved = !sameGeometry(now, then) || (oldEnergy == 0) != (newEnergy == 0);
        changed += moved ? std::max(oldEnergy, newEnergy) : difference * powerScale(now);
        totalOld += oldEnergy;
        totalNew += newEnergy;
      } else if (a < sorted.size() && (b == before.size() || keyOf(sorted[a]) < keyOf(before[b]))) {
        const double value = energies[a] = energy(sorted[a]);
        ++a;
        changed += value;
        totalNew += value;
      } else {
        const double value = m_temporalEnergy[b++];
        changed += value;
        totalOld += value;
      }
    }
    if (sorted.empty() != before.empty() || changed > .25 * std::max(totalOld, totalNew)) {
      ++lightingGeneration;
      m_temporalLights = sorted;
      m_temporalEnergy = std::move(energies);
    }
  }

  // Nodes are laid out with both children of a node adjacent: the descent in
  // light_bvh.glsl reads both at every level, so one memory neighborhood per
  // level instead of a far jump to every right subtree. With a dynamic part
  // the root's children are the two parts (nodes 1 and 2), the static part's
  // nodes follow, and the dynamic part's come last; leaf ranks and emitters
  // are in the same order (static, then dynamic).
  void LightBvh::build(std::vector<SphereLight> statics, std::vector<uint64_t> staticStamps,
                       std::vector<SphereLight> dynamics, std::vector<uint64_t> dynamicStamps, Reason reason) {
    m_writtenAll = true;
    m_split |= !dynamics.empty();
    m_staticReal = uint32_t(statics.size());
    m_dynamicReal = uint32_t(dynamics.size());
    m_lights = std::move(statics);
    m_stamps = std::move(staticStamps);
    if (m_split && m_lights.empty()) {
      m_lights.emplace_back();
      m_stamps.push_back(0);
    }
    m_staticCount = uint32_t(m_lights.size());
    m_present.assign(m_staticCount, 1);
    m_lights.insert(m_lights.end(), dynamics.begin(), dynamics.end());
    m_stamps.insert(m_stamps.end(), dynamicStamps.begin(), dynamicStamps.end());
    if (m_split && dynamics.empty()) {
      m_lights.emplace_back();
      m_stamps.push_back(0);
    }
    const uint32_t total = uint32_t(m_lights.size());
    m_emitters.resize(total);
    for (uint32_t i = 0; i < total; ++i)
      m_emitters[i] = emitterOf(m_lights[i]);
    m_nodes.clear();
    m_parents.clear();
    m_leaves.assign(total, 0);
    m_nodes.reserve(2 * size_t(total) - 1);
    depth = 0;
    std::vector<uint32_t> order(total);
    std::iota(order.begin(), order.end(), 0u);
    if (!m_split) {
      m_nodes.emplace_back();
      m_parents.push_back(0);
      buildInto(0, order, 0, total, 1);
      m_staticNodes = uint32_t(m_nodes.size());
      m_staticDepth = depth;
    } else {
      m_nodes.resize(3);
      m_parents.assign(3, 0);
      buildInto(1, order, 0, m_staticCount, 2);
      m_staticNodes = uint32_t(m_nodes.size());
      m_staticDepth = depth;
      buildInto(2, order, m_staticCount, total, 2);
      m_nodes[0].links[0] = 1;
      m_nodes[0].links[1] = 2;
      m_nodes[0].links[2] = 0;
      m_nodes[0].links[3] = total;
      bounds(0);
    }
    m_builtAt = m_update;
    ++builds;
    ++buildReasons[lastBuildReason = reason];
  }

  // The dynamic part again, from m_lights behind the static emitters. Static
  // nodes, emitters and leaf ranks stay as they are.
  void LightBvh::buildDynamic() {
    if (m_lights.size() == m_staticCount) {
      m_lights.emplace_back();
      m_stamps.push_back(0);
    }
    const uint32_t total = uint32_t(m_lights.size());
    m_emitters.resize(total);
    for (uint32_t i = m_staticCount; i < total; ++i) {
      m_emitters[i] = emitterOf(m_lights[i]);
      m_writtenEmitters.push_back(i);
    }
    m_nodes.resize(m_staticNodes);
    m_parents.resize(m_staticNodes);
    m_leaves.resize(total);
    m_nodes[2] = {};
    depth = m_staticDepth;
    std::vector<uint32_t> order(total);
    std::iota(order.begin(), order.end(), 0u);
    buildInto(2, order, m_staticCount, total, 2);
    m_nodes[0].links[3] = total;
    m_dirty.resize(m_nodes.size());
    std::fill(m_dirty.begin() + m_staticNodes, m_dirty.end(), 0);
    m_dirty[0] = 1;
  }

  void LightBvh::buildInto(uint32_t index, std::vector<uint32_t> &order, uint32_t begin, uint32_t end,
                           uint32_t level) {
    depth = std::max(depth, level);
    m_nodes[index].links[3] = end - begin;
    // Subtrees are not contiguous in node order (siblings are adjacent), so
    // each node records the first position of its leaves in leaf order
    // (links[0] of a leaf, links[2] of an internal node): subtree membership
    // is a leaf-rank range test (light_guiding.glsl lightRank).
    if (end - begin == 1) {
      m_nodes[index].links[0] = begin;
      m_nodes[index].links[2] = order[begin];
      m_leaves[order[begin]] = index;
    } else {
      float minimum[3], maximum[3];
      std::fill_n(minimum, 3, std::numeric_limits<float>::max());
      std::fill_n(maximum, 3, std::numeric_limits<float>::lowest());
      for (uint32_t i = begin; i < end; ++i)
        for (unsigned c = 0; c < 3; ++c) {
          minimum[c] = std::min(minimum[c], m_lights[order[i]].position[c]);
          maximum[c] = std::max(maximum[c], m_lights[order[i]].position[c]);
        }
      unsigned axis = 0;
      for (unsigned c = 1; c < 3; ++c)
        if (maximum[c] - minimum[c] > maximum[axis] - minimum[axis])
          axis = c;
      const uint32_t middle = begin + (end - begin) / 2;
      std::nth_element(order.begin() + begin, order.begin() + middle, order.begin() + end,
                       [&](uint32_t a, uint32_t b) {
                         return m_lights[a].position[axis] != m_lights[b].position[axis]
                                    ? m_lights[a].position[axis] < m_lights[b].position[axis]
                                    : a < b;
                       });
      const uint32_t left = uint32_t(m_nodes.size()), right = left + 1;
      m_nodes.emplace_back();
      m_nodes.emplace_back();
      m_parents.push_back(index);
      m_parents.push_back(index);
      m_nodes[index].links[0] = left;
      m_nodes[index].links[1] = right;
      m_nodes[index].links[2] = begin;
      buildInto(left, order, begin, middle, level + 1);
      buildInto(right, order, middle, end, level + 1);
    }
    bounds(index);
  }

  void LightBvh::bounds(uint32_t index) {
    if (!m_writtenAll)
      m_writtenNodes.push_back(index);
    auto &node = m_nodes[index];
    if (node.links[3] == 1) {
      const auto &light = m_lights[node.links[2]];
      for (unsigned c = 0; c < 3; ++c) {
        node.minimumPower[c] = light.position[c] - light.radius;
        node.maximum[c] = light.position[c] + light.radius;
      }
      node.maximum[3] = light.radius * light.radius;
      // Common 4*pi*pi cancels in every selection probability.
      node.minimumPower[3] =
          float(powerScale(light) *
                (.2126 * light.radiance[0] + .7152 * light.radiance[1] + .0722 * light.radiance[2]));
    } else {
      const auto &a = m_nodes[node.links[0]], &b = m_nodes[node.links[1]];
      if (a.minimumPower[3] == 0 || b.minimumPower[3] == 0) {
        // Keep zero-power slots for cheap refits without expanding the live
        // influence bounds around distant unlit redstone torches.
        const auto &live = a.minimumPower[3] > 0 ? a : b;
        std::copy_n(live.minimumPower, 3, node.minimumPower);
        std::copy_n(live.maximum, 4, node.maximum);
      } else {
        for (unsigned c = 0; c < 3; ++c) {
          node.minimumPower[c] = std::min(a.minimumPower[c], b.minimumPower[c]);
          node.maximum[c] = std::max(a.maximum[c], b.maximum[c]);
        }
        node.maximum[3] = std::max(a.maximum[3], b.maximum[3]);
      }
      node.minimumPower[3] = a.minimumPower[3] + b.minimumPower[3];
    }
  }

  double LightBvh::falloff(double distanceSquared, double range) {
    if (range <= 0)
      return 1;
    const auto t = std::clamp((distanceSquared / (range * range) - .5625) * (16. / 7.), 0., 1.);
    return (1 - t) * (1 - t) * (1 + 2 * t);
  }

  double LightBvh::weight(const LightNode &node, const float position[3], const float normal[3],
                          double range) {
    double distance2 = 0, radius2 = 0, toward = 0, nearest2 = 0;
    for (unsigned c = 0; c < 3; ++c) {
      const double half = .5 * (node.maximum[c] - node.minimumPower[c]);
      const double delta = .5 * (node.maximum[c] + node.minimumPower[c]) - position[c];
      distance2 += delta * delta;
      radius2 += half * half;
      toward += delta * normal[c];
      const double nearest = std::max(std::abs(delta) - half, 0.);
      nearest2 += nearest * nearest;
    }
    // Cull only when the entire enclosing sphere is below the receiving plane.
    if (toward + std::sqrt(radius2) <= 0)
      return 0;
    const double cosine =
        std::min(1., (toward + std::sqrt(radius2)) / std::sqrt(std::max({distance2, radius2, 1.e-12})));
    return falloff(nearest2, range) * node.minimumPower[3] * cosine /
           std::max({nearest2, .01 * radius2, double(node.maximum[3]), 1.e-12});
  }
} // namespace remix::scene
