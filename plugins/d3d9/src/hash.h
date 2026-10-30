#pragma once
#include "draw.h"

#include <cstddef>
#include <cstdint>

// The keys dxvk-remix computes, so that an RTX Remix game's rtx.conf texture
// lists and its USD mods (mat_<hash>, mesh_<hash>) apply unchanged.
namespace openremix::d3d9 {

// Texture hash of mip 0 as the game wrote it, rows packed at a 4-byte aligned
// pitch (dxvk-remix D3D9CommonTexture::SetupForRtxFrom): XXH3_64bits, or
// XXH64 with seed 0 for a lock upload with rtx.useObsoleteHashOnTextureUpload.
uint64_t textureHash(const uint8_t *packedMip0, size_t size, bool obsolete = false);

// The geometry asset hash of a triangle draw for a rule (config.h kHash*;
// dxvk-remix rtx_hashing.h getHashForRule): the rule's components in
// dxvk-remix's order, the first as it is and each later one chained with
// XXH64(&component, 8, hash).
//   positions: XXH3 chained over the position element of each referenced
//     vertex (indexed: the distinct indices, ascending; otherwise in order).
//   texcoords: the same over the texture coordinate set the draw samples.
//   indices: XXH3 of the indices less the smallest, at their own width
//     (0 for non-indexed draws).
//   geometrydescriptor: index count (0 non-indexed), vertex count, topology
//     and index type (VkPrimitiveTopology, VkIndexType: 0 non-indexed), chained.
//   vertexlayout: XXH3 of the vertex stride (size_t).
// 0 if the draw cannot be hashed; `degenerate` is set when all its indices
// are equal (dxvk-remix skips such draws).
uint64_t geometryHash(const Draw &draw, uint32_t rule, bool *degenerate = nullptr);

} // namespace openremix::d3d9
