#include "vertex.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace openremix::d3d9 {
namespace {

uint32_t elementSize(BYTE type) {
  switch (type) {
  case D3DDECLTYPE_FLOAT1: return 4;
  case D3DDECLTYPE_FLOAT2: return 8;
  case D3DDECLTYPE_FLOAT3: return 12;
  case D3DDECLTYPE_FLOAT4: return 16;
  case D3DDECLTYPE_D3DCOLOR:
  case D3DDECLTYPE_UBYTE4:
  case D3DDECLTYPE_UBYTE4N:
  case D3DDECLTYPE_SHORT2:
  case D3DDECLTYPE_SHORT2N:
  case D3DDECLTYPE_USHORT2N:
  case D3DDECLTYPE_UDEC3:
  case D3DDECLTYPE_DEC3N:
  case D3DDECLTYPE_FLOAT16_2: return 4;
  case D3DDECLTYPE_SHORT4:
  case D3DDECLTYPE_SHORT4N:
  case D3DDECLTYPE_USHORT4N:
  case D3DDECLTYPE_FLOAT16_4: return 8;
  default: return 0;
  }
}

float half(uint16_t h) {
  const uint32_t sign = uint32_t(h >> 15) << 31, exponent = (h >> 10) & 31, mantissa = h & 1023;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else { // subnormal
      float f = std::ldexp(float(mantissa), -24);
      return sign ? -f : f;
    }
  } else if (exponent == 31) {
    bits = sign | 0x7f800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// One element as four floats (missing components 0, w 1).
void read(BYTE type, const uint8_t *p, float out[4]) {
  out[0] = out[1] = out[2] = 0;
  out[3] = 1;
  switch (type) {
  case D3DDECLTYPE_FLOAT4: std::memcpy(out, p, 16); break;
  case D3DDECLTYPE_FLOAT3: std::memcpy(out, p, 12); break;
  case D3DDECLTYPE_FLOAT2: std::memcpy(out, p, 8); break;
  case D3DDECLTYPE_FLOAT1: std::memcpy(out, p, 4); break;
  case D3DDECLTYPE_D3DCOLOR: // B, G, R, A in memory; read as R, G, B, A
    out[0] = p[2] / 255.0f, out[1] = p[1] / 255.0f, out[2] = p[0] / 255.0f, out[3] = p[3] / 255.0f;
    break;
  case D3DDECLTYPE_UBYTE4:
    for (int i = 0; i < 4; ++i)
      out[i] = p[i];
    break;
  case D3DDECLTYPE_UBYTE4N:
    for (int i = 0; i < 4; ++i)
      out[i] = p[i] / 255.0f;
    break;
  case D3DDECLTYPE_SHORT2:
  case D3DDECLTYPE_SHORT4:
  case D3DDECLTYPE_SHORT2N:
  case D3DDECLTYPE_SHORT4N: {
    const int n = type == D3DDECLTYPE_SHORT2 || type == D3DDECLTYPE_SHORT2N ? 2 : 4;
    const bool normalized = type == D3DDECLTYPE_SHORT2N || type == D3DDECLTYPE_SHORT4N;
    for (int i = 0; i < n; ++i) {
      int16_t v;
      std::memcpy(&v, p + 2 * i, 2);
      out[i] = normalized ? std::max(v / 32767.0f, -1.0f) : float(v);
    }
    break;
  }
  case D3DDECLTYPE_USHORT2N:
  case D3DDECLTYPE_USHORT4N: {
    const int n = type == D3DDECLTYPE_USHORT2N ? 2 : 4;
    for (int i = 0; i < n; ++i) {
      uint16_t v;
      std::memcpy(&v, p + 2 * i, 2);
      out[i] = v / 65535.0f;
    }
    break;
  }
  case D3DDECLTYPE_UDEC3:
  case D3DDECLTYPE_DEC3N: {
    uint32_t v;
    std::memcpy(&v, p, 4);
    for (int i = 0; i < 3; ++i) {
      const uint32_t bits = (v >> (10 * i)) & 1023;
      if (type == D3DDECLTYPE_UDEC3)
        out[i] = float(bits);
      else
        out[i] = std::max(float(int32_t(bits << 22) >> 22) / 511.0f, -1.0f);
    }
    break;
  }
  case D3DDECLTYPE_FLOAT16_2:
  case D3DDECLTYPE_FLOAT16_4: {
    const int n = type == D3DDECLTYPE_FLOAT16_2 ? 2 : 4;
    for (int i = 0; i < n; ++i) {
      uint16_t v;
      std::memcpy(&v, p + 2 * i, 2);
      out[i] = half(v);
    }
    break;
  }
  default: break;
  }
}

// Where element `e` of vertex `vertex` lives, or nullptr outside the stream.
const uint8_t *at(const Draw &d, const D3DVERTEXELEMENT9 &e, uint32_t vertex) {
  if (e.Stream >= kStreams)
    return nullptr;
  const VertexStream &s = d.streams[e.Stream];
  const uint32_t size = elementSize(e.Type);
  if (!s.data || !size)
    return nullptr;
  const uint64_t offset = uint64_t(s.offset) + uint64_t(vertex) * s.stride + e.Offset;
  if (offset + size > s.size)
    return nullptr;
  return s.data + offset;
}

uint32_t packColor(const float c[4]) {
  uint32_t out = 0;
  for (int i = 0; i < 4; ++i)
    out |= uint32_t(std::clamp(c[i], 0.0f, 1.0f) * 255.0f + 0.5f) << (8 * i);
  return out;
}

} // namespace

std::vector<D3DVERTEXELEMENT9> elementsFromFvf(DWORD fvf) {
  std::vector<D3DVERTEXELEMENT9> out;
  WORD offset = 0;
  auto add = [&](BYTE type, BYTE usage, BYTE index) {
    out.push_back({0, offset, type, D3DDECLMETHOD_DEFAULT, usage, index});
    offset = WORD(offset + elementSize(type));
  };
  const DWORD position = fvf & D3DFVF_POSITION_MASK;
  int weights = 0;
  switch (position) {
  case D3DFVF_XYZ: add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0); break;
  case D3DFVF_XYZRHW: add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0); break;
  case D3DFVF_XYZW: add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITION, 0); break;
  case D3DFVF_XYZB1: weights = 1; break;
  case D3DFVF_XYZB2: weights = 2; break;
  case D3DFVF_XYZB3: weights = 3; break;
  case D3DFVF_XYZB4: weights = 4; break;
  case D3DFVF_XYZB5: weights = 5; break;
  default: return {};
  }
  if (position >= D3DFVF_XYZB1 && position <= D3DFVF_XYZB5) {
    add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0);
    const bool indices = (fvf & D3DFVF_LASTBETA_UBYTE4) || (fvf & D3DFVF_LASTBETA_D3DCOLOR);
    const int blendWeights = indices ? weights - 1 : weights;
    if (blendWeights > 0)
      add(BYTE(D3DDECLTYPE_FLOAT1 + blendWeights - 1), D3DDECLUSAGE_BLENDWEIGHT, 0);
    if (indices)
      add((fvf & D3DFVF_LASTBETA_D3DCOLOR) ? D3DDECLTYPE_D3DCOLOR : D3DDECLTYPE_UBYTE4, D3DDECLUSAGE_BLENDINDICES, 0);
  }
  if (fvf & D3DFVF_NORMAL)
    add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0);
  if (fvf & D3DFVF_PSIZE)
    add(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_PSIZE, 0);
  if (fvf & D3DFVF_DIFFUSE)
    add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0);
  if (fvf & D3DFVF_SPECULAR)
    add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1);
  const DWORD texCount = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
  for (DWORD i = 0; i < texCount && i < 8; ++i) {
    // D3DFVF_TEXCOORDSIZEn: 0 = 2 floats, 1 = 3, 2 = 4, 3 = 1.
    static const BYTE types[4] = {D3DDECLTYPE_FLOAT2, D3DDECLTYPE_FLOAT3, D3DDECLTYPE_FLOAT4, D3DDECLTYPE_FLOAT1};
    add(types[(fvf >> (16 + 2 * i)) & 3], D3DDECLUSAGE_TEXCOORD, BYTE(i));
  }
  out.push_back(D3DDECL_END());
  return out;
}

const D3DVERTEXELEMENT9 *findElement(const D3DVERTEXELEMENT9 *elements, BYTE usage, BYTE usageIndex) {
  if (!elements)
    return nullptr;
  for (const D3DVERTEXELEMENT9 *e = elements; e->Stream != 0xff; ++e)
    if (e->Usage == usage && e->UsageIndex == usageIndex)
      return e;
  return nullptr;
}

bool triangulate(const Draw &d, Triangles &out) {
  out.vertices.clear();
  uint32_t count; // vertices or indices the draw consumes
  switch (d.type) {
  case D3DPT_TRIANGLELIST: count = d.primCount * 3; break;
  case D3DPT_TRIANGLESTRIP:
  case D3DPT_TRIANGLEFAN: count = d.primCount + 2; break;
  default: return false; // points and lines have no surface
  }
  if (!d.primCount)
    return false;
  std::vector<uint32_t> sequence(count);
  if (d.indexed) {
    const uint32_t size = d.indices.is32 ? 4 : 2;
    const uint64_t first = uint64_t(d.startIndex) * size;
    if (!d.indices.data || first + uint64_t(count) * size > d.indices.size)
      return false;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t index = 0;
      std::memcpy(&index, d.indices.data + first + uint64_t(i) * size, size);
      const int64_t vertex = int64_t(d.baseVertex) + index;
      if (vertex < 0)
        return false;
      sequence[i] = uint32_t(vertex);
    }
  } else {
    for (uint32_t i = 0; i < count; ++i)
      sequence[i] = d.startVertex + i;
  }
  out.vertices.reserve(size_t(d.primCount) * 3);
  for (uint32_t t = 0; t < d.primCount; ++t) {
    uint32_t a, b, c;
    if (d.type == D3DPT_TRIANGLELIST) {
      a = sequence[3 * t], b = sequence[3 * t + 1], c = sequence[3 * t + 2];
    } else if (d.type == D3DPT_TRIANGLESTRIP) {
      // Odd strip triangles swap their first two vertices to keep the winding.
      a = sequence[t + (t & 1)], b = sequence[t + 1 - (t & 1)], c = sequence[t + 2];
    } else {
      a = sequence[0], b = sequence[t + 1], c = sequence[t + 2];
    }
    out.vertices.insert(out.vertices.end(), {a, b, c});
  }
  const auto [lo, hi] = std::minmax_element(out.vertices.begin(), out.vertices.end());
  out.low = *lo;
  out.high = *hi;
  return true;
}

bool decode(const Draw &d, const Triangles &t, Mesh &out) {
  out = Mesh();
  const D3DVERTEXELEMENT9 *position = findElement(d.elements, D3DDECLUSAGE_POSITIONT, 0);
  out.pretransformed = position != nullptr;
  if (!position)
    position = findElement(d.elements, D3DDECLUSAGE_POSITION, 0);
  if (!position || t.vertices.empty())
    return false;
  const D3DVERTEXELEMENT9 *normal = findElement(d.elements, D3DDECLUSAGE_NORMAL, 0);
  const D3DVERTEXELEMENT9 *color = findElement(d.elements, D3DDECLUSAGE_COLOR, 0);
  // The material's stage samples the texture coordinate set its
  // D3DTSS_TEXCOORDINDEX names.
  const BYTE set = BYTE(d.stage0()[D3DTSS_TEXCOORDINDEX] & 7);
  const D3DVERTEXELEMENT9 *texcoord = findElement(d.elements, D3DDECLUSAGE_TEXCOORD, set);
  const D3DVERTEXELEMENT9 *weight = findElement(d.elements, D3DDECLUSAGE_BLENDWEIGHT, 0);
  const D3DVERTEXELEMENT9 *blendIndex = findElement(d.elements, D3DDECLUSAGE_BLENDINDICES, 0);
  out.normals = normal != nullptr;
  out.colors = color != nullptr;

  const uint32_t count = t.high - t.low + 1;
  if (count > (1u << 24))
    return false;
  out.vertices.resize(count);
  if (out.pretransformed)
    out.rhw.resize(count);
  if (weight) {
    out.weightCount = weight->Type >= D3DDECLTYPE_FLOAT1 && weight->Type <= D3DDECLTYPE_FLOAT4
                          ? uint32_t(weight->Type - D3DDECLTYPE_FLOAT1 + 1)
                          : 4;
    out.weights.resize(size_t(count) * out.weightCount);
  }
  if (blendIndex)
    out.blendIndices.resize(size_t(count) * 4);
  // Every element taken is read in full: a draw whose reads would leave a
  // stream is not taken (Remix Plus D3D9Rtx::verticesInRange).
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t vertex = t.low + i;
    remixapi_HardcodedVertex &v = out.vertices[i];
    v = {};
    float value[4];
    const uint8_t *p = at(d, *position, vertex);
    if (!p)
      return false;
    read(position->Type, p, value);
    v.position[0] = value[0], v.position[1] = value[1], v.position[2] = value[2];
    if (out.pretransformed)
      out.rhw[i] = value[3];
    if (normal) {
      if (!(p = at(d, *normal, vertex)))
        return false;
      read(normal->Type, p, value);
      v.normal[0] = value[0], v.normal[1] = value[1], v.normal[2] = value[2];
    }
    if (texcoord) {
      if (!(p = at(d, *texcoord, vertex)))
        return false;
      read(texcoord->Type, p, value);
      v.texcoord[0] = value[0], v.texcoord[1] = value[1];
    }
    v.color = 0xffffffffu;
    if (color) {
      if (!(p = at(d, *color, vertex)))
        return false;
      read(color->Type, p, value);
      v.color = packColor(value);
    }
    if (weight) {
      if (!(p = at(d, *weight, vertex)))
        return false;
      read(weight->Type, p, value);
      for (uint32_t k = 0; k < out.weightCount; ++k)
        out.weights[size_t(i) * out.weightCount + k] = value[k];
    }
    if (blendIndex) {
      if (!(p = at(d, *blendIndex, vertex)))
        return false;
      // UBYTE4 in order; D3DCOLOR as B, G, R, A (the fixed-function pipeline's).
      const bool color4 = blendIndex->Type == D3DDECLTYPE_D3DCOLOR;
      for (uint32_t k = 0; k < 4; ++k)
        out.blendIndices[size_t(i) * 4 + k] = color4 ? p[k == 3 ? 3 : 2 - k] : p[k];
    }
  }
  out.indices.resize(t.vertices.size());
  for (size_t i = 0; i < t.vertices.size(); ++i)
    out.indices[i] = t.vertices[i] - t.low;
  return true;
}

} // namespace openremix::d3d9
