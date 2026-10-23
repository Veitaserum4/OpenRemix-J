#include "scene/light_bvh.h"
#include "scene/light_visibility.h"
#include "scene/resources.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <random>
#include <remix/remix_native_ext.h>

namespace {
void require(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}
} // namespace
int main() {
  using namespace remix::scene;
  // A buffer holding an older generation, patched with the ranges written
  // since (changesSince), holds the current emitters and nodes exactly:
  // static lights that animate, and dynamic lights that come and go.
  {
    std::mt19937 random(77);
    const auto make = [&](uint64_t id) {
      SphereLight light;
      light.id = id;
      light.triangle = 1;
      light.radius = .2f;
      for (unsigned c = 0; c < 3; ++c) {
        light.position[c] = float(random() % 1000) / 10;
        light.radiance[c] = 1 + float(random() % 10);
        for (unsigned v = 0; v < 3; ++v)
          light.corners[v][c] = light.position[c] + (v == c ? .5f : 0);
      }
      return light;
    };
    std::vector<SphereLight> base;
    for (uint64_t i = 0; i < 400; ++i)
      base.push_back(make(i + 1));
    LightBvh bvh;
    for (int frame = 0; frame < 200; ++frame)
      bvh.update(base); // settled: one static part
    struct Held {
      uint64_t generation;
      std::vector<LightEmitter> emitters;
      std::vector<LightNode> nodes;
    };
    std::vector<Held> held;
    size_t partials = 0;
    std::vector<LightBvh::Range> emitterRanges, nodeRanges;
    for (int frame = 0; frame < 120; ++frame) {
      auto lights = base;
      lights[size_t(frame * 7) % lights.size()].radiance[0] = float(frame % 5); // animate one static light
      for (int k = 0; k < frame % 4; ++k)
        lights.push_back(make((uint64_t(1) << 40) + uint64_t(frame * 10 + k))); // transient lights
      bvh.update(lights);
      for (size_t s = 0; s < held.size(); ++s) {
        auto &h = held[s];
        if (s != size_t(frame) % 3 || h.generation == bvh.generation)
          continue;
        if (bvh.changesSince(h.generation, emitterRanges, nodeRanges)) {
          // As the GPU buffers: sized for the current tree, older contents kept.
          h.emitters.resize(bvh.emitters().size());
          h.nodes.resize(bvh.nodes().size());
          for (const auto &[first, end] : emitterRanges)
            std::copy(bvh.emitters().begin() + first, bvh.emitters().begin() + end, h.emitters.begin() + first);
          for (const auto &[first, end] : nodeRanges)
            std::copy(bvh.nodes().begin() + first, bvh.nodes().begin() + end, h.nodes.begin() + first);
          ++partials;
        } else {
          h.emitters = bvh.emitters();
          h.nodes = bvh.nodes();
        }
        h.generation = bvh.generation;
        require(std::memcmp(h.emitters.data(), bvh.emitters().data(), h.emitters.size() * sizeof(LightEmitter)) == 0 &&
                    std::memcmp(h.nodes.data(), bvh.nodes().data(), h.nodes.size() * sizeof(LightNode)) == 0,
                "partial light tree uploads reproduce the tree");
      }
      // Three slots, one rendered per frame: each lags by up to three generations.
      if (held.size() < 3)
        held.push_back({bvh.generation, bvh.emitters(), bvh.nodes()});
    }
    require(partials > 0, "unchanged-size refits upload partially");
  }
  // Sparse animation and input reordering must produce the same GPU records
  // as a fresh hierarchy, including when whole branches lose their power.
  {
    std::mt19937 random(2080);
    std::vector<SphereLight> lights(101);
    for (size_t i = 0; i < lights.size(); ++i) {
      auto &light = lights[i];
      light.id = i + 1;
      light.triangle = uint32_t(i % 3);
      light.radius = .1f + float(i % 7) * .05f;
      for (unsigned c = 0; c < 3; ++c) {
        light.position[c] = float(random() % 1000) / 10;
        light.radiance[c] = float(random() % 100);
        for (unsigned v = 0; v < 3; ++v)
          light.corners[v][c] = light.position[c] + (v == c ? .5f : 0);
      }
    }
    LightBvh cached;
    for (unsigned frame = 0; frame < 256; ++frame) {
      if (frame % 7 == 0)
        std::shuffle(lights.begin(), lights.end(), random);
      for (unsigned edit = 0; edit < 5; ++edit) {
        auto &light = lights[random() % lights.size()];
        for (auto &channel : light.radiance)
          channel = frame % 4 ? float(random() % 100) : 0;
      }
      if (frame == 80)
        for (auto &light : lights)
          std::fill_n(light.radiance, 3, 0.f);
      if (frame == 128)
        for (auto &light : lights) {
          light.radius *= 2;
          light.corners[1][0] += .75f;
        }
      cached.update(lights);
      LightBvh rebuilt;
      rebuilt.update(lights);
      require(cached.active == rebuilt.active && cached.nodes().size() == rebuilt.nodes().size(),
              "Sparse light refit changed hierarchy membership");
      require(std::memcmp(cached.nodes().data(), rebuilt.nodes().data(),
                          cached.nodes().size() * sizeof(LightNode)) == 0,
              "Sparse light refit changed bounds or sampling power");
      require(std::memcmp(cached.emitters().data(), rebuilt.emitters().data(),
                          cached.emitters().size() * sizeof(LightEmitter)) == 0,
              "Cached light ordering changed emitter records");
      const auto generation = cached.generation;
      std::shuffle(lights.begin(), lights.end(), random);
      require(!cached.update(lights) && cached.generation == generation,
              "Reordered identical lights invalidated the hierarchy");
    }
    require(cached.builds == 1 && cached.refits > 200,
            "Radiance animation unnecessarily rebuilt the hierarchy");
  }
  require(LightBvh::falloff(1.e12, 0) == 1 && LightBvh::falloff(144, 16) == 1 &&
              LightBvh::falloff(256, 16) == 0 && LightBvh::falloff(400, 16) == 0,
          "Influence bound changed inner light or failed to reach black");
  double previousFalloff = 1;
  for (unsigned i = 0; i <= 1000; ++i) {
    const double distance = 12 + i * .004;
    const double fade = LightBvh::falloff(distance * distance, 16);
    require(fade >= 0 && fade <= previousFalloff, "Influence fade is not monotonic");
    previousFalloff = fade;
  }
  require(1 - LightBvh::falloff(12.001 * 12.001, 16) < 1.e-6 &&
              LightBvh::falloff(15.999 * 15.999, 16) < 1.e-6,
          "Influence fade has an endpoint discontinuity");
  {
    SphereLight live{}, unlit{};
    live.id = 1; live.radius = .08f; live.radiance[0] = 1;
    unlit.id = 2; unlit.radius = .08f; unlit.position[0] = 100;
    LightBvh bounds;
    bounds.update({live, unlit});
    require(bounds.nodes()[0].maximum[0] < .1f,
            "Unlit slots expanded the live influence bound");
    unlit.radiance[0] = 1;
    bounds.update({live, unlit});
    require(bounds.builds == 1 && bounds.refits == 1 && bounds.nodes()[0].maximum[0] > 100,
            "Reactivated emitter did not restore its bound by refitting");
  }
  remix::NativeResources resources;
  remixapi_LightInfoSphereEXT sphere{};
  sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
  sphere.radius = .08f;
  remixapi_LightInfo info{};
  info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  info.pNext = &sphere;
  info.hash = 10;
  info.radiance = {200, 110, 44};
  remixapi_LightHandle handle{};
  resources.createLight(info, handle);
  resources.drawLight(handle);
  resources.drawLight(handle);
  auto accepted = resources.sphereLights();
  require(accepted.size() == 1, "Repeated draw duplicated emitter power");
  auto rejected = [&] {
    bool failed = false;
    try {
      resources.updateLight(handle, info);
    } catch (...) {
      failed = true;
    }
    require(failed && resources.sphereLights() == accepted,
            "Malformed sphere modified accepted state");
  };
  sphere.radius = -.1f;
  rejected();
  sphere.radius = std::numeric_limits<float>::infinity();
  rejected();
  sphere.radius = .08f;
  sphere.position.x = std::numeric_limits<float>::quiet_NaN();
  rejected();
  sphere.position.x = 0;
  info.radiance.y = -1;
  rejected();
  info.radiance.y = 110;
  sphere.shaping_hasvalue = 1;
  rejected();
  sphere.shaping_hasvalue = 0;
  sphere.radius = 0;
  resources.updateLight(handle, info);
  require(resources.sphereLights().empty(), "Zero-radius compatibility placeholder emitted light");
  sphere.radius = .08f;
  resources.updateLight(handle, info);
  require(resources.sphereLights() == accepted, "Finite-radius update did not restore the emitter");
  resources.finishFrame();
  require(resources.sphereLights().empty(),
          "Lights survived an absent submission");
  resources.drawLight(handle);
  resources.destroyLight(handle);
  require(resources.sphereLights().empty(),
          "Destroyed light survived draw selection");

  for (unsigned count : {1u, 100u, 1000u, 5000u}) {
    std::vector<SphereLight> lights;
    for (unsigned i = 0; i < count; ++i) {
      SphereLight light;
      light.id = i + 1;
      light.radius = .08f;
      light.position[0] = float(i % 71) - 35;
      light.position[1] = float(i / 71) - 35;
      light.position[2] = 4;
      light.radiance[0] = 1 + i % 13;
      lights.push_back(light);
    }
    LightBvh bvh;
    require(bvh.update(lights), "Initial BVH not built");
    require(bvh.active == count && bvh.nodes().size() == 2 * count - 1 &&
                bvh.depth <= unsigned(std::ceil(std::log2(count))) + 1,
            "Unbounded BVH depth or wrong membership");
    std::reverse(lights.begin(), lights.end());
    require(!bvh.update(lights) && bvh.builds == 1,
            "Unchanged reordered lights rebuilt BVH");
    const float p[] = {0, 0, 0}, n[] = {0, 0, 1};
    std::vector<double> pdf(count);
    std::function<void(uint32_t, double)> visit = [&](uint32_t node,
                                                      double probability) {
      const auto &value = bvh.nodes()[node];
      if (value.links[3] == 1) {
        pdf[value.links[2]] = probability;
        return;
      }
      const auto a = LightBvh::weight(bvh.nodes()[value.links[0]], p, n);
      const auto b = LightBvh::weight(bvh.nodes()[value.links[1]], p, n);
      const auto split = std::clamp(a / (a + b), 1.e-6, 1. - 1.e-6);
      visit(value.links[0], probability * split);
      visit(value.links[1], probability * (1 - split));
    };
    visit(0, 1);
    // Any leaf with positive physical attenuation must keep nonzero sampling
    // support at every enclosing node, including nodes near a range boundary.
    std::function<void(uint32_t, bool)> bounded = [&](uint32_t node, bool supported) {
      const auto &value = bvh.nodes()[node];
      supported &= LightBvh::weight(value, p, n, 16) > 0;
      if (value.links[3] == 1) {
        const auto &emitter = bvh.emitters()[value.links[2]];
        double d2 = 0;
        for (unsigned c = 0; c < 3; ++c)
          d2 += double(emitter.positionRadius[c]) * emitter.positionRadius[c];
        require(LightBvh::falloff(d2, 16) == 0 || supported,
                "Influence bounds culled a contributing emitter");
      } else {
        bounded(value.links[0], supported);
        bounded(value.links[1], supported);
      }
    };
    bounded(0, true);
    double sum = 0;
    for (unsigned i = 0; i < count; ++i) {
      require(pdf[i] > 0 && std::isfinite(pdf[i]),
              "A contributing light has zero selection PDF");
      sum += pdf[i];
    }
    require(std::abs(sum - 1) < 1.e-10, "BVH traversal did not reach every "
                                        "emitter with normalized probability");
    for (auto &light : lights)
      light.radiance[0] *= .5f;
    require(bvh.update(lights) && bvh.refits == 1 && bvh.builds == 1,
            "Power change rebuilt topology");
    for (auto &light : lights)
      light.position[0] += 128;
    require(bvh.update(lights) && bvh.refits == 2,
            "Origin shift did not refit bounds");
    for (auto &light : lights)
      light.radiance[0] = 0;
    bvh.update(lights);
    require(!bvh.active && bvh.nodes()[0].minimumPower[3] == 0,
            "Zero-power slots still contribute");
    bvh.update({});
    require(bvh.nodes().empty() && bvh.emitters().empty(),
            "Unloaded lights remained in hierarchy");
    std::printf("%u lights: balanced build, exact PDF sum, power/origin refit, "
                "removal passed\n",
                count);
  }
  // Animated emission updates transport every tick without continually
  // shortening global lighting history. Slow ramps compare to an anchor.
  {
    SphereLight triangle{};
    triangle.id = 1;
    triangle.triangle = 2;
    triangle.radius = 4;
    triangle.radiance[0] = 50;
    triangle.position[2] = -.1f;
    triangle.corners[0][0] = -2; triangle.corners[0][1] = -2;
    triangle.corners[1][0] = 2; triangle.corners[1][1] = -2;
    triangle.corners[2][1] = 2;
    for (auto &corner : triangle.corners) corner[2] = -.1f;
    LightBvh bvh;
    bvh.update({triangle});
    const auto stable = bvh.lightingGeneration;
    const auto visibility = bvh.visibilityGeneration;
    for (int frame = 0; frame < 100; ++frame) {
      triangle.radiance[0] = frame & 1 ? 50.5f : 49.5f;
      require(bvh.update({triangle}), "Animated radiance failed to update transport");
      require(bvh.emitters()[0].radiance[0] == triangle.radiance[0] && bvh.lightingGeneration == stable,
              "Small emission animation invalidated temporal lighting");
    }
    triangle.radiance[0] = 53;
    bvh.update({triangle});
    require(bvh.lightingGeneration == stable, "Small intensity step discarded history");
    triangle.radiance[0] = 56;
    bvh.update({triangle});
    require(bvh.lightingGeneration == stable, "Modest animated variation discarded history");
    triangle.radiance[0] = 68;
    bvh.update({triangle});
    require(bvh.lightingGeneration == stable + 1, "Gradual ramp never triggered lighting response");
    require(bvh.visibilityGeneration == visibility, "Color animation discarded visibility evidence");
    triangle.corners[0][2] += .01f;
    bvh.update({triangle});
    require(bvh.lightingGeneration == stable + 2, "Moving emitter retained stale temporal lighting");
    require(bvh.visibilityGeneration == visibility + 1, "Moving emitter retained visibility history");
    triangle.radiance[0] = 0;
    bvh.update({triangle});
    require(bvh.lightingGeneration == stable + 3, "Extinguished light retained history");
    bvh.update({});
    require(bvh.lightingGeneration == stable + 4, "Removed light retained temporal membership");
    SphereLight dominant = triangle, animated = triangle;
    dominant.id = 10; dominant.radiance[0] = 100;
    animated.id = 11; animated.radiance[0] = 1;
    bvh.update({dominant, animated});
    const auto aggregate = bvh.lightingGeneration;
    for (int frame = 0; frame < 20; ++frame) {
      animated.radiance[0] = frame & 1 ? 2.f : 1.f;
      bvh.update({dominant, animated});
      require(bvh.lightingGeneration == aggregate,
              "One animated emitter shortened the whole scene's history");
    }
  }
  // A receiver inside a volume of lights must not almost exclusively sample
  // whichever internal bounding box contains it. Measure the full discrete
  // estimator against the analytic sphere integral, before visibility or NRD.
  {
    std::vector<SphereLight> lights;
    for (int x = -5; x <= 5; ++x)
      for (int y = -5; y <= 5; ++y)
        for (int z = -5; z <= 5; ++z) {
          SphereLight light{};
          light.id = lights.size() + 1;
          light.radius = .08f;
          light.position[0] = 2 * x + .5f;
          light.position[1] = 2 * y + .75f;
          light.position[2] = 2 * z + .25f;
          light.radiance[0] = 200;
          light.radiance[1] = 110;
          light.radiance[2] = 44;
          lights.push_back(light);
        }
    LightBvh bvh;
    bvh.update(lights);
    for (float offset : {0.f, .5f, 1.f, 2.f}) {
      const float p[] = {offset, 0, 0}, n[] = {0, 0, 1};
      std::vector<double> pdf(lights.size());
      std::function<void(uint32_t, double)> visit = [&](uint32_t i,
                                                        double probability) {
        const auto &node = bvh.nodes()[i];
        if (node.links[3] == 1) {
          pdf[node.links[2]] = probability;
          return;
        }
        const double a = LightBvh::weight(bvh.nodes()[node.links[0]], p, n, 16);
        const double b = LightBvh::weight(bvh.nodes()[node.links[1]], p, n, 16);
        const double split = a == 0 ? 0
                             : b == 0
                                 ? 1
                                 : std::clamp(a / (a + b), 1.e-6, 1. - 1.e-6);
        visit(node.links[0], probability * split);
        visit(node.links[1], probability * (1 - split));
      };
      visit(0, 1);
      double reference = 0, expectation = 0, secondMoment = 0, maximum = 0,
             totalPdf = 0;
      for (size_t i = 0; i < lights.size(); ++i) {
        const auto &light = lights[i];
        double distance2 = 0;
        for (unsigned c = 0; c < 3; ++c)
          distance2 += std::pow(double(light.position[c]) - p[c], 2);
        // These spheres are entirely above or below the receiving plane.
        const double contribution =
            (.2126 * 200 + .7152 * 110 + .0722 * 44) * double(light.radius) *
            light.radius * std::max(light.position[2], 0.f) /
            std::pow(distance2, 1.5) * LightBvh::falloff(distance2, 16);
        totalPdf += pdf[i];
        reference += contribution;
        if (contribution == 0)
          continue;
        require(pdf[i] > 0, "Interior receiver lost a contributing light");
        const double estimate = contribution / pdf[i];
        expectation += pdf[i] * estimate;
        secondMoment += pdf[i] * estimate * estimate;
        maximum = std::max(maximum, estimate);
      }
      require(std::abs(totalPdf - 1) < 1.e-10 &&
                  std::abs(expectation - reference) < 1.e-10,
              "Interior light selection changed the sphere integral");
      const double relativeDeviation =
          std::sqrt(secondMoment - reference * reference) / reference;
      require(relativeDeviation < 4 && maximum / reference < 80,
              "Interior light cluster produced excessive inverse-PDF outliers");
    }
    std::puts(
        "1331 torches: interior receiver energy and selection variance passed");
  }
  // Lights that come and go, and the emitters of dynamic instances, live in
  // the hierarchy's dynamic part: the static part keeps its indices (no
  // build), and the visibility table, which covers the static nodes only,
  // stays as it is.
  {
    std::mt19937 random(1531);
    auto partition = std::make_shared<VisibilityPartition>();
    // A 4x4 grid of cells in x/y (0..40), one cluster each; a cell sees its
    // column (and, not symmetrically, its row to the right).
    std::function<int32_t(int, int, int, int)> split = [&](int x0, int x1, int y0, int y1) -> int32_t {
      if (x1 - x0 == 1 && y1 - y0 == 1) {
        partition->leafClusters.push_back(int32_t(partition->leafClusters.size()));
        return -int32_t(partition->leafClusters.size());
      }
      const bool alongX = x1 - x0 >= y1 - y0;
      const int middle = alongX ? (x0 + x1) / 2 : (y0 + y1) / 2;
      const int32_t index = int32_t(partition->nodes.size());
      partition->nodes.emplace_back();
      // In front of the plane: the upper half.
      const int32_t front = alongX ? split(middle, x1, y0, y1) : split(x0, x1, middle, y1);
      const int32_t back = alongX ? split(x0, middle, y0, y1) : split(x0, x1, y0, middle);
      VisibilityPartition::Node n{};
      n.plane[alongX ? 0 : 1] = 1;
      n.plane[3] = middle * 10.f;
      n.children[0] = front;
      n.children[1] = back;
      partition->nodes[index] = n;
      return index;
    };
    split(0, 4, 0, 4);
    partition->clusterCount = 16;
    partition->rowBytes = 2;
    partition->visibility.assign(32, 0);
    const auto cell = [&](int cluster) {
      // Where the tree puts the cluster: probe its cells.
      for (int x = 0; x < 4; ++x)
        for (int y = 0; y < 4; ++y) {
          const float p[3] = {x * 10 + 5.f, y * 10 + 5.f, 0};
          if (partition->cluster(p) == cluster)
            return std::pair{x, y};
        }
      return std::pair{-1, -1};
    };
    for (int a = 0; a < 16; ++a)
      for (int b = 0; b < 16; ++b) {
        const auto [ax, ay] = cell(a);
        const auto [bx, by] = cell(b);
        require(ax >= 0 && bx >= 0, "Test partition does not cover its grid");
        if (ax == bx || (ay == by && bx > ax))
          partition->visibility[a * 2 + b / 8] |= uint8_t(1u << (b % 8));
      }
    partition->version = 1;
    std::vector<SphereLight> lamps;
    for (unsigned i = 0; i < 40; ++i) {
      SphereLight light;
      light.id = 100 + i;
      light.radius = .1f;
      light.position[0] = float(random() % 400) / 10;
      light.position[1] = float(random() % 400) / 10;
      light.radiance[0] = 1 + float(i % 5);
      lamps.push_back(light);
    }
    const auto prop = [&](uint64_t instance, float x, unsigned count) {
      std::vector<SphereLight> lights;
      for (unsigned i = 0; i < count; ++i) {
        SphereLight light;
        light.id = instance * 1000 + i;
        light.triangle = 1;
        light.dynamic = true;
        light.radiance[1] = 2;
        for (unsigned v = 0; v < 3; ++v) {
          light.corners[v][0] = x + float(i % 5) + (v == 1);
          light.corners[v][1] = 20 + float(i / 5) + (v == 2);
          for (unsigned c = 0; c < 3; ++c)
            light.position[c] += light.corners[v][c] / 3;
        }
        light.radius = 1;
        lights.push_back(light);
      }
      return lights;
    };
    LightBvh bvh;
    LightVisibility visibility;
    std::vector<LightNode> staticNodes;
    // The table against every (cluster, node) pair computed from the lights.
    const auto checkTable = [&](const char *message) {
      const auto &nodes = bvh.nodes();
      const auto &table = *visibility.table();
      require(visibility.tableNodes() == bvh.staticNodes() &&
                  table.size() == 16 * size_t(visibility.rowWords()),
              "Visibility table does not cover the static nodes");
      std::vector<uint8_t> bits(nodes.size());
      for (int from = 0; from < 16; ++from) {
        for (size_t n = nodes.size(); n-- > 0;) {
          const auto &links = nodes[n].links;
          if (links[3] == 1) {
            const auto &light = bvh.lights()[links[2]];
            const int cluster = partition->cluster(light.position);
            bits[n] = links[2] >= bvh.staticEmitters() || cluster < 0 || partition->visible(from, cluster);
          } else {
            bits[n] = bits[links[0]] | bits[links[1]];
          }
        }
        for (uint32_t n = 0; n < visibility.tableNodes(); ++n)
          require(((table[from * visibility.rowWords() + n / 32] >> (n % 32)) & 1) == bits[n], message);
      }
    };
    const auto update = [&](std::vector<SphereLight> lights) {
      std::shuffle(lights.begin(), lights.end(), random);
      const bool changed = bvh.update(lights);
      visibility.update(partition, bvh, true, .1f);
      const auto &nodes = bvh.nodes();
      require(nodes.empty() || (nodes.size() == 2 * bvh.emitters().size() - 1 &&
                                nodes[0].links[3] == bvh.emitters().size()),
              "Two-part hierarchy is not a full binary tree");
      uint32_t lit = 0;
      for (const auto &light : lights)
        lit += light.radiance[0] > 0 || light.radiance[1] > 0 || light.radiance[2] > 0;
      require(bvh.active == lit, "Hierarchy lights differ from the submitted ones");
      return changed;
    };
    auto frame = lamps;
    update(frame);
    require(bvh.builds == 1 && bvh.staticNodes() == bvh.nodes().size() && bvh.dynamicLights == 0,
            "Static lights alone need no dynamic part");
    checkTable("Initial visibility table differs from the partition");
    // The first emitters of a dynamic instance add the dynamic part: once.
    for (unsigned step = 0; step < 300; ++step) {
      frame = lamps;
      // The prop moves every frame, changes identity every 7 frames, is away
      // for 5 of every 40 frames and has a second copy for some.
      if (step % 40 < 35) {
        const auto first = prop(1 + step / 7, float(step % 30), 12);
        frame.insert(frame.end(), first.begin(), first.end());
        if (step % 3 == 0) {
          const auto second = prop(900, 30 - float(step % 30), 7);
          frame.insert(frame.end(), second.begin(), second.end());
        }
      }
      const uint64_t tables = visibility.version();
      update(frame);
      if (step == 0) {
        require(bvh.builds == 2 && bvh.lastBuildReason == LightBvh::FirstDynamic,
                "The dynamic part was not added by one build");
        staticNodes.assign(bvh.nodes().begin() + 3, bvh.nodes().begin() + bvh.staticNodes());
        checkTable("Visibility table differs after the dynamic part was added");
      } else {
        require(bvh.builds == 2, "Dynamic-instance lights rebuilt the hierarchy");
        require(visibility.version() == tables, "Dynamic-instance lights remade the visibility table");
        require(bvh.staticNodes() == staticNodes.size() + 3 &&
                    std::memcmp(staticNodes.data(), bvh.nodes().data() + 3,
                                staticNodes.size() * sizeof(LightNode)) == 0,
                "Dynamic-instance lights changed static nodes");
      }
      require(bvh.staticLights == lamps.size() && bvh.instanceLights == frame.size() - lamps.size() &&
                  bvh.dynamicLights == bvh.instanceLights,
              "Dynamic-instance lights are not all in the dynamic part");
      const float p[] = {20, 20, 5}, n[] = {0, 0, -1};
      double total = 0;
      std::function<void(uint32_t, double)> visit = [&](uint32_t node, double probability) {
        const auto &value = bvh.nodes()[node];
        if (value.links[3] == 1) {
          total += probability;
          return;
        }
        const double a = LightBvh::weight(bvh.nodes()[value.links[0]], p, n);
        const double b = LightBvh::weight(bvh.nodes()[value.links[1]], p, n);
        if (a + b <= 0)
          return;
        visit(value.links[0], probability * a / (a + b));
        visit(value.links[1], probability * b / (a + b));
      };
      visit(0, 1);
      require(std::abs(total - 1) < 1.e-10, "Two-part traversal lost selection probability");
    }
    require(bvh.dynamicBuilds > 40 && bvh.refits >= 250, "Dynamic part was not rebuilt on its own");
    checkTable("Visibility table differs after dynamic lights came and went");
    // A static light that leaves keeps its slot and returns to it.
    frame = lamps;
    frame.erase(frame.begin() + 7);
    update(frame);
    require(bvh.builds == 2 && bvh.retiredSlots == 1 && bvh.active == lamps.size() - 1,
            "A static light that left rebuilt the hierarchy or kept its power");
    update(lamps);
    require(bvh.builds == 2 && bvh.retiredSlots == 0 && bvh.active == lamps.size() &&
                std::memcmp(staticNodes.data(), bvh.nodes().data() + 3, staticNodes.size() * sizeof(LightNode)) == 0,
            "A returning static light did not take its slot");
    // A new light of no dynamic instance waits in the dynamic part and joins
    // the static part once at rest; a moving one stays.
    SphereLight added = lamps[0], carried = lamps[1];
    added.id = 5000;
    added.position[0] = 33;
    carried.id = 5001;
    const uint64_t refits = bvh.refits;
    // (While another new light keeps moving, the one at rest waits four times as long.)
    for (unsigned step = 0; step < 5 * LightBvh::kSettle && bvh.builds == 2; ++step) {
      frame = lamps;
      frame.push_back(added);
      carried.position[1] = float(step % 40);
      frame.push_back(carried);
      update(frame);
      require(bvh.builds > 2 || bvh.dynamicLights == 2, "New lights did not wait in the dynamic part");
    }
    require(bvh.builds == 3 && bvh.lastBuildReason == LightBvh::Settled && bvh.staticLights == lamps.size() + 1 &&
                bvh.dynamicLights == 1 && bvh.refits - refits >= LightBvh::kSettle,
            "A light at rest did not join the static part by one build");
    checkTable("Visibility table differs after a light joined the static part");
    // A static light that moves is visible from everywhere from then on.
    frame.back() = carried;
    frame[3].position[0] += 15;
    const uint64_t tables = visibility.version();
    update(frame);
    require(bvh.builds == 3 && visibility.version() == tables + 1 && visibility.unrestricted >= 2,
            "A moved static light kept its clusters");
    update({});
    require(bvh.nodes().empty() && bvh.lastBuildReason == LightBvh::Cleared && !visibility.table(),
            "Unloaded lights remained in the two-part hierarchy");
    std::puts("Dynamic part: static indices, visibility table, slots and settling passed");
  }
}
