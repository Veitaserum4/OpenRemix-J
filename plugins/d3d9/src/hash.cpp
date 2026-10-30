#include "hash.h"

#include "config.h"
#include "vertex.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <vector>

#define XXH_INLINE_ALL
#include "xxhash.h"

namespace openremix::d3d9 {
namespace {

// An element's bytes in the stream (dxvk-remix: its Vulkan format's size).
uint32_t typeSize(BYTE type) {
  switch (type) {
  case D3DDECLTYPE_FLOAT1: return 4;
  case D3DDECLTYPE_FLOAT2: return 8;
  case D3DDECLTYPE_FLOAT3: return 12;
  case D3DDECLTYPE_FLOAT4: return 16;
  case D3DDECLTYPE_SHORT4:
  case D3DDECLTYPE_SHORT4N:
  case D3DDECLTYPE_USHORT4N:
  case D3DDECLTYPE_FLOAT16_4: return 8;
  case D3DDECLTYPE_UNUSED: return 0;
  default: return 4;
  }
}

// One vertex region (dxvk-remix HashQuery): an element in its stream.
struct Region {
  const uint8_t *base = nullptr; // the element of vertex 0
  int64_t first = 0;             // offset of base in the stream's data
  uint32_t stride = 0, size = 0;
  uint64_t streamSize = 0;
};

bool region(const Draw &d, const D3DVERTEXELEMENT9 *element, int64_t firstVertex, Region &out) {
  if (!element || element->Stream >= kStreams)
    return false;
  const VertexStream &s = d.streams[element->Stream];
  if (!s.data)
    return false;
  out.first = int64_t(s.offset) + int64_t(s.stride) * firstVertex + element->Offset;
  out.base = s.data;
  out.stride = s.stride;
  out.size = typeSize(element->Type);
  out.streamSize = s.size;
  return out.size != 0;
}

// XXH3 chained over the region's vertices: `vertices` (relative to the
// region's first vertex), or the first `count` in order when null.
bool hashRegion(const Region &r, const std::vector<uint32_t> *vertices, uint32_t count, uint64_t &out) {
  uint64_t hash = 0;
  auto add = [&](uint32_t vertex) {
    const int64_t at = r.first + int64_t(vertex) * r.stride;
    if (at < 0 || uint64_t(at) + r.size > r.streamSize)
      return false;
    hash = XXH3_64bits_withSeed(r.base + at, r.size, hash);
    return true;
  };
  if (vertices) {
    for (const uint32_t v : *vertices)
      if (!add(v))
        return false;
  } else {
    for (uint32_t v = 0; v < count; ++v)
      if (!add(v))
        return false;
  }
  out = hash;
  return true;
}

} // namespace

uint64_t textureHash(const uint8_t *packedMip0, size_t size, bool obsolete) {
  return obsolete ? XXH64(packedMip0, size, 0) : XXH3_64bits(packedMip0, size);
}

uint64_t geometryHash(const Draw &d, uint32_t rule, bool *degenerate) {
  if (degenerate)
    *degenerate = false;
  if (!d.elements || !rule)
    return 0;
  uint32_t count, topology; // indices (or vertices) consumed, VkPrimitiveTopology
  switch (d.type) {
  case D3DPT_TRIANGLELIST:
    count = d.primCount * 3;
    topology = 3;
    break;
  case D3DPT_TRIANGLESTRIP:
    count = d.primCount + 2;
    topology = 4;
    break;
  case D3DPT_TRIANGLEFAN:
    count = d.primCount + 2;
    topology = 5;
    break;
  default:
    return 0;
  }
  if (!d.primCount)
    return 0;

  uint64_t indexHash = 0;
  uint32_t vertexCount, indexCount = 0, indexType = 0;
  int64_t firstVertex;
  std::vector<uint32_t> unique;
  if (d.indexed) {
    const uint32_t indexSize = d.indices.is32 ? 4 : 2;
    const uint64_t first = uint64_t(d.startIndex) * indexSize, bytes = uint64_t(count) * indexSize;
    if (!d.indices.data || first + bytes > d.indices.size)
      return 0;
    std::vector<uint32_t> values(count);
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t v = 0;
      std::memcpy(&v, d.indices.data + first + size_t(i) * indexSize, indexSize);
      values[i] = v;
    }
    const auto [lo, hi] = std::minmax_element(values.begin(), values.end());
    const uint32_t low = *lo, high = *hi;
    if (low == high) {
      if (degenerate)
        *degenerate = true;
      return 0;
    }
    // The indices less the smallest, at their own width.
    std::vector<uint8_t> rebased(bytes);
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t v = values[i] - low;
      std::memcpy(rebased.data() + size_t(i) * indexSize, &v, indexSize);
    }
    indexHash = XXH3_64bits(rebased.data(), rebased.size());
    // The distinct indices, ascending (a bitmap over [low, high]).
    std::vector<uint64_t> seen((size_t(high - low) >> 6) + 1, 0);
    for (const uint32_t v : values)
      seen[(v - low) >> 6] |= 1ull << ((v - low) & 63);
    for (size_t word = 0; word < seen.size(); ++word)
      for (uint64_t bits = seen[word]; bits; bits &= bits - 1)
        unique.push_back(uint32_t(word * 64 + std::countr_zero(bits)));
    vertexCount = high - low + 1;
    indexCount = count;
    indexType = d.indices.is32 ? 1 : 0;
    firstVertex = int64_t(d.baseVertex) + low;
  } else {
    vertexCount = count;
    firstVertex = d.startVertex;
  }

  const D3DVERTEXELEMENT9 *position = findElement(d.elements, D3DDECLUSAGE_POSITION, 0);
  if (!position)
    position = findElement(d.elements, D3DDECLUSAGE_POSITIONT, 0);
  Region positions;
  uint64_t positionHash = 0;
  if (!region(d, position, firstVertex, positions) ||
      !hashRegion(positions, d.indexed ? &unique : nullptr, vertexCount, positionHash))
    return 0;

  // The components in dxvk-remix's order (HashComponents): positions,
  // texcoords, indices, geometry descriptor, vertex layout.
  uint64_t components[5] = {};
  bool used[5] = {};
  if (rule & kHashPositions)
    components[0] = positionHash, used[0] = true;
  if (rule & kHashTexcoords) {
    const BYTE set = BYTE(d.stage0()[D3DTSS_TEXCOORDINDEX] & 7);
    Region texcoords;
    uint64_t texcoordHash = 0;
    if (region(d, findElement(d.elements, D3DDECLUSAGE_TEXCOORD, set), firstVertex, texcoords))
      hashRegion(texcoords, d.indexed ? &unique : nullptr, vertexCount, texcoordHash);
    components[1] = texcoordHash, used[1] = true;
  }
  if (rule & kHashIndices)
    components[2] = indexHash, used[2] = true;
  if (rule & kHashDescriptor) {
    uint64_t h = XXH3_64bits_withSeed(&indexCount, 4, 0);
    h = XXH3_64bits_withSeed(&vertexCount, 4, h);
    h = XXH3_64bits_withSeed(&topology, 4, h);
    components[3] = XXH3_64bits_withSeed(&indexType, 4, h), used[3] = true;
  }
  if (rule & kHashLayout) {
    const uint64_t stride = positions.stride; // size_t in the 64-bit runtime
    components[4] = XXH3_64bits(&stride, sizeof(stride)), used[4] = true;
  }
  uint64_t asset = 0;
  bool firstComponent = true;
  for (int i = 0; i < 5; ++i) {
    if (!used[i])
      continue;
    asset = firstComponent ? components[i] : XXH64(&components[i], 8, asset);
    firstComponent = false;
  }
  return asset;
}

} // namespace openremix::d3d9
