#pragma once
#include "draw.h"

#include <remix/remix_c.h>

#include <vector>

// Vertex input: the fixed-function vertex format as declaration elements,
// a draw's primitives as a triangle list, and its vertices as Remix vertices.
namespace openremix::d3d9 {

// The declaration D3D9 builds for an FVF code (D3DXDeclaratorFromFVF),
// terminated by D3DDECL_END. Empty if the code is not a valid FVF.
std::vector<D3DVERTEXELEMENT9> elementsFromFvf(DWORD fvf);

// The draw's primitives as triangles: `vertices` are vertex numbers in the
// streams (base vertex and start vertex applied), three per triangle, with
// degenerate strip and fan triangles kept (D3D draws nothing for them either).
struct Triangles {
  std::vector<uint32_t> vertices;
  uint32_t low = 0, high = 0; // smallest and largest vertex number
};
bool triangulate(const Draw &draw, Triangles &out);

// The referenced vertices [low, high] as Remix vertices, positions as the
// streams hold them (object space, or screen pixels for POSITIONT), with
// indices rebased onto `low`. Colors are RGBA (R in the low byte).
struct Mesh {
  std::vector<remixapi_HardcodedVertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<float> rhw; // POSITIONT w per vertex (pre-transformed draws only)
  // Fixed-function vertex blending inputs, `weightCount` and 4 per vertex:
  // BLENDWEIGHT's components and BLENDINDICES' four (D3DCOLOR in B, G, R, A
  // order, as the fixed-function pipeline reads them).
  std::vector<float> weights;
  std::vector<uint32_t> blendIndices;
  uint32_t weightCount = 0;
  bool normals = false, colors = false, pretransformed = false;
};
bool decode(const Draw &draw, const Triangles &triangles, Mesh &out);

// The element with this usage, or nullptr.
const D3DVERTEXELEMENT9 *findElement(const D3DVERTEXELEMENT9 *elements, BYTE usage, BYTE usageIndex);

} // namespace openremix::d3d9
