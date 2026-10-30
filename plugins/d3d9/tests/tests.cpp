// Unit tests of the plugin's engine-independent parts.
#include "bridge/channel.h"
#include "camera.h"
#include "classify.h"
#include "config.h"
#include "formats.h"
#include "hash.h"
#include "vertex.h"
#include "vertex_capture.h"

#include <d3dcompiler.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#define XXH_INLINE_ALL
#include "xxhash.h"

namespace {

using namespace openremix::d3d9;

int g_failures = 0;
#define CHECK(condition)                                                                                         \
  do {                                                                                                           \
    if (!(condition)) {                                                                                          \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition);                          \
      ++g_failures;                                                                                              \
    }                                                                                                            \
  } while (0)

void fvf() {
  const auto e = elementsFromFvf(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX2);
  CHECK(e.size() == 6);
  CHECK(e[0].Usage == D3DDECLUSAGE_POSITION && e[0].Offset == 0 && e[0].Type == D3DDECLTYPE_FLOAT3);
  CHECK(e[1].Usage == D3DDECLUSAGE_NORMAL && e[1].Offset == 12);
  CHECK(e[2].Usage == D3DDECLUSAGE_COLOR && e[2].Offset == 24 && e[2].Type == D3DDECLTYPE_D3DCOLOR);
  CHECK(e[3].Usage == D3DDECLUSAGE_TEXCOORD && e[3].UsageIndex == 0 && e[3].Offset == 28);
  CHECK(e[4].Usage == D3DDECLUSAGE_TEXCOORD && e[4].UsageIndex == 1 && e[4].Offset == 36);
  CHECK(e[5].Stream == 0xff);

  const auto rhw = elementsFromFvf(D3DFVF_XYZRHW | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0));
  CHECK(rhw[0].Usage == D3DDECLUSAGE_POSITIONT && rhw[0].Type == D3DDECLTYPE_FLOAT4);
  CHECK(rhw[1].Type == D3DDECLTYPE_FLOAT3 && rhw[1].Offset == 16);

  const auto skinned = elementsFromFvf(D3DFVF_XYZB3 | D3DFVF_LASTBETA_UBYTE4);
  CHECK(skinned[1].Usage == D3DDECLUSAGE_BLENDWEIGHT && skinned[1].Type == D3DDECLTYPE_FLOAT2);
  CHECK(skinned[2].Usage == D3DDECLUSAGE_BLENDINDICES && skinned[2].Offset == 20);
  CHECK(elementsFromFvf(0).empty());
}

struct Vertex {
  float x, y, z;
  uint32_t color; // D3DCOLOR (A8R8G8B8)
  float u, v;
};

Draw quadDraw(const std::vector<Vertex> &vertices, const std::vector<uint16_t> &indices,
              std::vector<D3DVERTEXELEMENT9> &elements) {
  elements = elementsFromFvf(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
  Draw d;
  d.elements = elements.data();
  d.streams[0] = {reinterpret_cast<const uint8_t *>(vertices.data()), uint32_t(vertices.size() * sizeof(Vertex)), 0,
                  sizeof(Vertex)};
  d.indexed = true;
  d.indices = {reinterpret_cast<const uint8_t *>(indices.data()), uint32_t(indices.size() * 2), false};
  d.type = D3DPT_TRIANGLELIST;
  d.primCount = uint32_t(indices.size() / 3);
  d.renderStates[D3DRS_COLORWRITEENABLE] = 15;
  d.renderStates[D3DRS_ZWRITEENABLE] = TRUE;
  d.projection._44 = 0; // perspective
  d.targetWidth = d.backBufferWidth = 1280;
  d.targetHeight = d.backBufferHeight = 720;
  d.targetFormat = D3DFMT_X8R8G8B8;
  d.stages[0].states[D3DTSS_COLOROP] = D3DTOP_MODULATE;
  d.stages[0].states[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
  d.stages[0].states[D3DTSS_COLORARG2] = D3DTA_CURRENT;
  d.stages[0].states[D3DTSS_ALPHAOP] = D3DTOP_SELECTARG1;
  d.stages[0].states[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
  d.stages[1].states[D3DTSS_COLOROP] = D3DTOP_DISABLE;
  return d;
}

void triangles() {
  Draw d;
  d.type = D3DPT_TRIANGLESTRIP;
  d.startVertex = 10;
  d.primCount = 3;
  Triangles t;
  CHECK(triangulate(d, t));
  // Strip winding: (10 11 12) (12 11 13) (12 13 14).
  const std::vector<uint32_t> strip = {10, 11, 12, 12, 11, 13, 12, 13, 14};
  CHECK(t.vertices == strip);
  CHECK(t.low == 10 && t.high == 14);

  d.type = D3DPT_TRIANGLEFAN;
  d.startVertex = 0;
  d.primCount = 2;
  CHECK(triangulate(d, t));
  const std::vector<uint32_t> fan = {0, 1, 2, 0, 2, 3};
  CHECK(t.vertices == fan);

  std::vector<uint32_t> indices = {0, 1, 2, 2, 1, 3};
  d.type = D3DPT_TRIANGLELIST;
  d.indexed = true;
  d.baseVertex = 5;
  d.startIndex = 3;
  d.primCount = 1;
  d.indices = {reinterpret_cast<const uint8_t *>(indices.data()), uint32_t(indices.size() * 4), true};
  CHECK(triangulate(d, t));
  const std::vector<uint32_t> list = {7, 6, 8};
  CHECK(t.vertices == list);
  d.startIndex = 4; // past the end of the index buffer
  CHECK(!triangulate(d, t));
  d.type = D3DPT_LINELIST;
  CHECK(!triangulate(d, t));
}

void decoding() {
  const std::vector<Vertex> vertices = {
      {0, 0, 0, 0xff102030, 0, 0}, {1, 0, 0, 0x80405060, 1, 0}, {1, 1, 0, 0xffffffff, 1, 1}};
  const std::vector<uint16_t> indices = {0, 1, 2};
  std::vector<D3DVERTEXELEMENT9> elements;
  Draw d = quadDraw(vertices, indices, elements);
  Triangles t;
  Mesh m;
  CHECK(triangulate(d, t));
  CHECK(decode(d, t, m));
  CHECK(m.vertices.size() == 3 && m.indices.size() == 3);
  CHECK(!m.pretransformed && m.colors && !m.normals);
  CHECK(m.vertices[1].position[0] == 1 && m.vertices[2].texcoord[1] == 1);
  // A8R8G8B8 0xff102030 is R 0x10, G 0x20, B 0x30: RGBA with R in the low byte.
  CHECK(m.vertices[0].color == 0xff302010u);
  CHECK(m.vertices[1].color == 0x80605040u);
}

void textures() {
  // 3x2 L8: rows of 3 bytes packed at a 4-byte pitch, the padding zero.
  const uint8_t l8[] = {1, 2, 3, 9, 9, 9, 9, 9, 4, 5, 6, 9};
  const auto packed = packLevel(D3DFMT_L8, 3, 2, l8, 8);
  const std::vector<uint8_t> expected = {1, 2, 3, 0, 4, 5, 6, 0};
  CHECK(packed == expected);
  CHECK(textureHash(packed.data(), packed.size()) == XXH3_64bits(packed.data(), packed.size()));
  // rtx.useObsoleteHashOnTextureUpload: XXH64, seed 0.
  CHECK(textureHash(packed.data(), packed.size(), true) == XXH64(packed.data(), packed.size(), 0));

  // A8R8G8B8 in memory is B, G, R, A.
  const uint8_t argb[] = {0x30, 0x20, 0x10, 0x80};
  Converted c;
  CHECK(convertLevel(D3DFMT_A8R8G8B8, 1, 1, argb, true, c));
  CHECK(c.rgba8 && c.format == REMIXAPI_FORMAT_R8G8B8A8_SRGB);
  CHECK(c.data[0] == 0x10 && c.data[1] == 0x20 && c.data[2] == 0x30 && c.data[3] == 0x80);
  CHECK(convertLevel(D3DFMT_X8R8G8B8, 1, 1, argb, false, c) && c.data[3] == 255);
  const uint8_t red565[] = {0x00, 0xf8, 0, 0}; // 0xf800, padded to 4 bytes
  CHECK(convertLevel(D3DFMT_R5G6B5, 1, 1, red565, false, c));
  CHECK(c.data[0] == 255 && c.data[1] == 0 && c.data[2] == 0 && c.data[3] == 255);

  // A BC1 block of one colour (c0 = c1 = white, 4-colour mode).
  const uint8_t block[8] = {0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0};
  const auto rgba = decodeBC1(block, 4, 4);
  CHECK(rgba.size() == 64 && rgba[0] == 255 && rgba[63] == 255);
  CHECK(convertLevel(D3DFMT_DXT1, 4, 4, block, true, c) && !c.rgba8 && c.data.size() == 8);
  CHECK(!convertLevel(D3DFMT_D24S8, 4, 4, block, true, c));
}

// The dxvk-remix asset hash of a draw, computed here from its definition
// (rtx_hashing.cpp, d3d9_rtx_geometry.cpp) for float3 positions at the start
// of a `stride`-byte vertex.
uint64_t referenceHash(const uint8_t *vertices, uint32_t stride, const std::vector<uint32_t> &order,
                       const void *rebased, size_t rebasedBytes, uint32_t indexCount, uint32_t vertexCount,
                       uint32_t topology, uint32_t indexType) {
  uint64_t positions = 0;
  for (uint32_t v : order)
    positions = XXH3_64bits_withSeed(vertices + size_t(v) * stride, 12, positions);
  const uint64_t indices = rebased ? XXH3_64bits(rebased, rebasedBytes) : 0;
  uint64_t descriptor = XXH3_64bits_withSeed(&indexCount, 4, 0);
  descriptor = XXH3_64bits_withSeed(&vertexCount, 4, descriptor);
  descriptor = XXH3_64bits_withSeed(&topology, 4, descriptor);
  descriptor = XXH3_64bits_withSeed(&indexType, 4, descriptor);
  uint64_t asset = positions;
  asset = XXH64(&indices, 8, asset);
  return XXH64(&descriptor, 8, asset);
}

void geometry() {
  const uint32_t rule = kHashPositions | kHashIndices | kHashDescriptor;
  const std::vector<Vertex> vertices = {
      {9, 9, 9, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0}, {1, 1, 0, 0, 0, 0}, {0, 1, 0, 0, 0, 0}};
  // Indices 1..4: rebased onto 1, the distinct ones hashed ascending.
  const std::vector<uint16_t> indices = {3, 1, 2, 1, 3, 4};
  std::vector<D3DVERTEXELEMENT9> elements;
  Draw d = quadDraw(vertices, indices, elements);
  const uint64_t hash = geometryHash(d, rule);
  const std::vector<uint16_t> rebased = {2, 0, 1, 0, 2, 3};
  const auto *bytes = reinterpret_cast<const uint8_t *>(vertices.data());
  CHECK(hash == referenceHash(bytes + sizeof(Vertex), sizeof(Vertex), {0, 1, 2, 3}, rebased.data(), 12, 6, 4, 3, 0));
  // Texture coordinates and colours are not part of the default rule.
  std::vector<Vertex> recoloured = vertices;
  recoloured[2].color = 0xff00ff00;
  recoloured[2].u = 7;
  std::vector<D3DVERTEXELEMENT9> elements2;
  CHECK(geometryHash(quadDraw(recoloured, indices, elements2), rule) == hash);
  // With texcoords in the rule, they are.
  const uint32_t withTexcoords = rule | kHashTexcoords;
  CHECK(geometryHash(quadDraw(recoloured, indices, elements2), withTexcoords) != geometryHash(d, withTexcoords));
  // Positions are.
  std::vector<Vertex> moved = vertices;
  moved[4].y = 2;
  CHECK(geometryHash(quadDraw(moved, indices, elements2), rule) != hash);
  // A base vertex moves the hashed positions, not the indices.
  Draw based = d;
  based.baseVertex = -1;
  CHECK(geometryHash(based, rule) ==
        referenceHash(bytes, sizeof(Vertex), {0, 1, 2, 3}, rebased.data(), 12, 6, 4, 3, 0));
  // Non-indexed: every vertex in order from the start vertex, indices 0
  // mixed in, a descriptor of index count and type 0.
  Draw list = d;
  list.indexed = false;
  list.startVertex = 1;
  list.primCount = 1;
  CHECK(geometryHash(list, rule) == referenceHash(bytes + sizeof(Vertex), sizeof(Vertex), {0, 1, 2}, nullptr, 0, 0, 3,
                                                  3, 0));
  Draw strip = list;
  strip.type = D3DPT_TRIANGLESTRIP;
  strip.primCount = 2;
  CHECK(geometryHash(strip, rule) ==
        referenceHash(bytes + sizeof(Vertex), sizeof(Vertex), {0, 1, 2, 3}, nullptr, 0, 0, 4, 4, 0));
  // All indices equal: dxvk-remix drops the draw.
  const std::vector<uint16_t> flat = {1, 1, 1};
  bool degenerate = false;
  CHECK(geometryHash(quadDraw(vertices, flat, elements2), rule, &degenerate) == 0 && degenerate);
  // One component alone is that component: the positions' chain.
  uint64_t positions = 0;
  for (uint32_t v = 1; v <= 4; ++v)
    positions = XXH3_64bits_withSeed(bytes + size_t(v) * sizeof(Vertex), 12, positions);
  CHECK(geometryHash(d, kHashPositions) == positions);
  CHECK(parseHashRule("positions, indices,geometrydescriptor") == rule);
  CHECK(parseHashRule("Positions,TexCoords,VertexLayout") == (kHashPositions | kHashTexcoords | kHashLayout));
}

void configuration() {
  Config c;
  parseConfig("# comment\n"
              "rtx.uiTextures = 0x10, 0x20\n"
              "rtx.skyBoxTextures = 0xAB\n"
              "rtx.ignoreTextures = 0xAB, 0xCD # trailing comment\n"
              "rtx.orthographicIsUI = False\n"
              "rtx.zUp = True\n"
              "rtx.sceneScale = 2.5\n"
              "rtx.pathTracing.maxBounces = 4\n"
              "openremix.backend = pathtrace\n",
              c);
  CHECK(c.uiTextures.count(0x10) && c.uiTextures.count(0x20));
  CHECK(c.categories[0xAB] == (REMIXAPI_INSTANCE_CATEGORY_BIT_SKY | REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE));
  CHECK(c.categories[0xCD] == REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE);
  CHECK(!c.orthographicIsUI && c.zUp && c.sceneScale == 2.5f);
  CHECK(c.passthrough.size() == 1 && c.passthrough[0].first == "rtx.pathTracing.maxBounces");
  CHECK(c.backend == "pathtrace");
  CHECK(std::abs(metersPerUnit(c) - 0.004f) < 1e-6f);
  // A later file's list replaces the earlier one.
  parseConfig("rtx.skyBoxTextures = 0xEF\n", c);
  CHECK(c.categories[0xAB] == REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE);
  CHECK(c.categories[0xEF] == REMIXAPI_INSTANCE_CATEGORY_BIT_SKY);
}

void classification() {
  const std::vector<Vertex> vertices = {{0, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0}, {1, 1, 0, 0, 0, 0}};
  const std::vector<uint16_t> indices = {0, 1, 2};
  std::vector<D3DVERTEXELEMENT9> elements;
  Draw d = quadDraw(vertices, indices, elements);
  Config c;
  CHECK(classify(d, c, false) == DrawKind::World);
  CHECK(classify(d, c, true) == DrawKind::Ui);
  c.preTransformedVerticesIsUI = false;
  CHECK(classify(d, c, true) == DrawKind::Forward);

  // A render target of another size than the back buffer (dxvk-remix compares sizes).
  Draw offscreen = d;
  offscreen.targetWidth = 512;
  CHECK(classify(offscreen, c, false) == DrawKind::Forward);
  Draw sameSize = d;
  sameSize.backBuffer = false;
  CHECK(classify(sameSize, c, false) == DrawKind::World);
  Draw depthOnly = d;
  depthOnly.renderStates[D3DRS_COLORWRITEENABLE] = D3DCOLORWRITEENABLE_ALPHA | D3DCOLORWRITEENABLE_RED;
  CHECK(classify(depthOnly, c, false) == DrawKind::Forward);
  Draw shader = d;
  shader.vertexShader = true;
  CHECK(classify(shader, c, false) == DrawKind::Forward);
  Draw query = d;
  query.occlusionQuery = true;
  CHECK(classify(query, c, false) == DrawKind::Forward);
  Draw ranged = d;
  ranged.drawId = 5;
  c.drawCallRangeFirst = 6;
  CHECK(classify(ranged, c, false) == DrawKind::Skip);
  c.drawCallRangeFirst = 0;

  // A shadow mask: an untextured fill of a small square target of a
  // non-colour format.
  Draw mask = d;
  mask.stages[0].states[D3DTSS_COLOROP] = D3DTOP_SELECTARG1;
  mask.stages[0].states[D3DTSS_COLORARG1] = D3DTA_TFACTOR;
  mask.targetWidth = mask.targetHeight = 256;
  mask.targetFormat = D3DFMT_R32F;
  CHECK(classify(mask, c, false) == DrawKind::Skip);
  mask.targetFormat = D3DFMT_A8R8G8B8;
  CHECK(classify(mask, c, false) == DrawKind::Forward); // only another target
  // A stencil shadow volume.
  Draw volume = d;
  volume.renderStates[D3DRS_STENCILENABLE] = TRUE;
  volume.renderStates[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
  volume.renderStates[D3DRS_STENCILZFAIL] = D3DSTENCILOP_INCR;
  volume.renderStates[D3DRS_ZWRITEENABLE] = FALSE;
  CHECK(classify(volume, c, false) == DrawKind::Skip);

  Draw ortho = d;
  ortho.projection._44 = 1;
  ortho.renderStates[D3DRS_ZWRITEENABLE] = FALSE;
  CHECK(classify(ortho, c, false) == DrawKind::Ui);
  ortho.renderStates[D3DRS_ZWRITEENABLE] = TRUE;
  CHECK(classify(ortho, c, false) == DrawKind::World);

  // A UI texture on any stage.
  Draw textured = d;
  textured.stages[1].hash = 0x42;
  c.uiTextures.insert(0x42);
  CHECK(classify(textured, c, false) == DrawKind::Ui);
  textured.texture0 = 0x43;
  textured.stages[1].hash = 0;
  c.categories[0x43] = REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE;
  CHECK(classify(textured, c, false) == DrawKind::Skip);

  Draw blended = d;
  blended.renderStates[D3DRS_ALPHABLENDENABLE] = TRUE;
  c.enableAlphaBlend = false;
  CHECK(classify(blended, c, false) == DrawKind::Skip);
}

void textureSelection() {
  const std::vector<Vertex> vertices = {{0, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0}, {1, 1, 0, 0, 0, 0}};
  const std::vector<uint16_t> indices = {0, 1, 2};
  std::vector<D3DVERTEXELEMENT9> elements;
  Config c;
  Draw d = quadDraw(vertices, indices, elements);
  // No texture: no stage, the draw is taken.
  CHECK(selectTexture(d, c) && d.textureStage == -1 && d.texture0 == 0);
  // Stage 0 with a texture.
  d.stages[0].bound = d.stages[0].texture2D = true;
  d.stages[0].hash = 0x10;
  CHECK(selectTexture(d, c) && d.textureStage == 0 && d.texture0 == 0x10);
  // A lightmap on stage 0 (set 1) and the base texture on stage 1 (set 0):
  // the lowest set wins, lightmaps never.
  d.stages[0].states[D3DTSS_TEXCOORDINDEX] = 1;
  c.lightmapTextures.insert(0x10);
  d.stages[1].bound = d.stages[1].texture2D = true;
  d.stages[1].hash = 0x20;
  d.stages[1].states[D3DTSS_COLOROP] = D3DTOP_MODULATE;
  d.stages[1].states[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
  d.stages[1].states[D3DTSS_COLORARG2] = D3DTA_CURRENT;
  d.stages[2].states[D3DTSS_COLOROP] = D3DTOP_DISABLE;
  CHECK(selectTexture(d, c) && d.textureStage == 1 && d.texture0 == 0x20);
  CHECK(d.stage0() == d.stages[1].states);
  // A stage that does not read its texture is not taken.
  d.stages[1].states[D3DTSS_COLORARG1] = D3DTA_DIFFUSE;
  d.stages[1].states[D3DTSS_ALPHAOP] = D3DTOP_DISABLE;
  CHECK(selectTexture(d, c) && d.textureStage == -1);
  // The chosen texture without a hash: dxvk-remix skips the draw.
  d.stages[1].states[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
  d.stages[1].hash = 0;
  CHECK(!selectTexture(d, c));
  // A cube map only with rtx.allowCubemaps.
  d.stages[1].hash = 0x30;
  d.stages[1].texture2D = false;
  CHECK(selectTexture(d, c) && d.textureStage == -1);
  c.allowCubemaps = true;
  CHECK(selectTexture(d, c) && d.textureStage == 1);
}

void cameras() {
  const std::vector<Vertex> vertices = {{0, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0}, {1, 1, 0, 0, 0, 0}};
  const std::vector<uint16_t> indices = {0, 1, 2};
  std::vector<D3DVERTEXELEMENT9> elements;
  Config c;
  Draw d = quadDraw(vertices, indices, elements);
  auto perspective = [](float fov) {
    D3DMATRIX p = {};
    p._22 = 1 / std::tan(fov / 2);
    p._11 = p._22 / 1.5f;
    p._33 = 1.001f, p._34 = 1, p._43 = -0.1f;
    return p;
  };
  for (int i = 0; i < 4; ++i)
    d.world.m[i][i] = d.view.m[i][i] = 1;
  d.view._43 = 5; // a view that is not identity
  d.projection = perspective(1.0f);
  d.viewport.MaxZ = 1;
  float fov, shear;
  decomposeProjection(d.projection, fov, shear);
  CHECK(std::fabs(fov - 1.0f) < 1e-5f && shear == 0);

  CameraFrame frame;
  bool updates;
  CHECK(classifyCamera(d, c, false, false, frame, updates) == CameraClass::Main && updates && frame.mainSet);
  // Another field of view: a view model, when they are on.
  Draw weapon = d;
  weapon.projection = perspective(0.7f);
  CHECK(classifyCamera(weapon, c, false, false, frame, updates) == CameraClass::Main);
  c.viewModelEnable = true;
  CHECK(classifyCamera(weapon, c, false, false, frame, updates) == CameraClass::ViewModel);
  // A viewport up to rtx.viewModel.maxZThreshold.
  Draw shallow = d;
  shallow.viewport.MaxZ = 0.1f;
  c.viewModelMaxZThreshold = 0.1f;
  CHECK(classifyCamera(shallow, c, false, false, frame, updates) == CameraClass::ViewModel);
  // No camera: an identity projection, a sheared one.
  Draw flat = d;
  flat.projection = {};
  for (int i = 0; i < 4; ++i)
    flat.projection.m[i][i] = 1;
  CHECK(classifyCamera(flat, c, false, false, frame, updates) == CameraClass::Unknown && !updates);
  CHECK(classifyCamera(flat, c, true, false, frame, updates) == CameraClass::Sky);
  Draw sheared = d;
  sheared.projection._31 = 0.5f;
  CHECK(classifyCamera(sheared, c, false, false, frame, updates) == CameraClass::Unknown);
  // A skinned draw once a Main camera is known sets none up.
  CHECK(classifyCamera(d, c, false, true, frame, updates) == CameraClass::Main && !updates);

  // Sky rules.
  CHECK(!skyDraw(d, c, 0));
  CHECK(skyDraw(d, c, REMIXAPI_INSTANCE_CATEGORY_BIT_SKY));
  Draw distant = d;
  distant.viewport.MinZ = 1;
  CHECK(skyDraw(distant, c, 0));
  c.skyDrawcallIdThreshold = 3;
  d.drawId = 2;
  CHECK(skyDraw(d, c, 0));
  d.texture0 = 0x50;
  CHECK(!skyDraw(d, c, 0));
}

void profilesLayering() {
  // HL2 (Portal RTX's profile): dxvk-remix's defaults for hl2.exe, under
  // the game's own rtx.conf.
  const auto dir = std::filesystem::temp_directory_path() / "openremix-d3d9-profile-test";
  std::filesystem::create_directories(dir);
  { std::ofstream(dir / "rtx.conf") << "rtx.zUp = False\n"; }
  Config c = loadConfig(dir, dir / "hl2.exe");
  CHECK(c.useObsoleteHashOnTextureUpload && c.allowCubemaps && c.viewModelEnable);
  CHECK(!c.zUp); // rtx.conf wins
  CHECK(!c.lightmapTextures.empty() && !c.uiTextures.empty());
  Config other = loadConfig(dir, dir / "game.exe");
  CHECK(!other.useObsoleteHashOnTextureUpload && other.uiTextures.empty());
  std::filesystem::remove_all(dir);
}

// The same shader compiled for vs_1_1, vs_2_0 and vs_3_0 (vs_1_1 and vs_2_0
// translated to vs_3_0), run by vertex capture on Direct3D's software vertex
// processor: each gives the clip-space positions computed here. It reads a
// constant by a relative address (vs_1_1: mov a0; vs_2_0: mova) and uses
// sincos (vs_2_0's takes helper constants). Skipped without a D3D9 device or
// d3dcompiler_47.dll.
void vertexCaptureModels() {
  static const char kSource[] = R"(
float4x4 m : register(c0);
float4 offsets[4] : register(c4);
struct I { float3 p : POSITION; float2 uv : TEXCOORD0; float index : TEXCOORD1; };
struct O { float4 p : POSITION; float4 c : COLOR0; float2 uv : TEXCOORD0; };
O main(I i) {
  O o;
  float3 p = i.p + offsets[(int)i.index].xyz;
  float s, c;
  sincos(i.uv.x, s, c);
  p.y += s * 0.5;
  o.p = mul(float4(p, 1), m);
  o.c = float4(1, 0.5, 0.25, 1);
  o.uv = i.uv * 2;
  return o;
}
)";
  HMODULE library = LoadLibraryW(L"d3dcompiler_47.dll");
  auto compile = library ? reinterpret_cast<pD3DCompile>(GetProcAddress(library, "D3DCompile")) : nullptr;
  // The system's d3d9.dll: the plugin built next to this executable would
  // load in its place.
  wchar_t system[MAX_PATH];
  GetSystemDirectoryW(system, MAX_PATH);
  using Create = IDirect3D9 *(WINAPI *)(UINT);
  HMODULE d3d9 = LoadLibraryW((std::wstring(system) + L"\\d3d9.dll").c_str());
  auto create = d3d9 ? reinterpret_cast<Create>(GetProcAddress(d3d9, "Direct3DCreate9")) : nullptr;
  IDirect3D9 *d3d = create ? create(D3D_SDK_VERSION) : nullptr;
  HWND window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = window;
  IDirect3DDevice9 *game = nullptr;
  if (!compile || !d3d ||
      FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &game))) {
    std::printf("vertex capture test skipped (no D3D9 device or d3dcompiler_47.dll)\n");
    if (d3d)
      d3d->Release();
    return;
  }

  struct In {
    float x, y, z, u, v, index;
  };
  const In vertices[4] = {{0, 0, 1, 0.1f, 0, 0}, {1, 0, 2, 0.7f, 0, 1}, {0, 1, 3, 1.3f, 1, 2}, {1, 1, 4, 2.1f, 1, 3}};
  const std::vector<D3DVERTEXELEMENT9> elements = {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
                                                   {0, 12, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0},
                                                   {0, 20, D3DDECLTYPE_FLOAT1, 0, D3DDECLUSAGE_TEXCOORD, 1},
                                                   D3DDECL_END()};
  // A perspective-like matrix (w from z), row-vector convention, and offsets.
  const float m[4][4] = {{1.5f, 0, 0, 0}, {0, 2, 0, 0}, {0.1f, 0.2f, 1.01f, 1}, {0.3f, -0.4f, -0.1f, 0}};
  float constants[256 * 4] = {};
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      constants[r * 4 + c] = m[c][r]; // HLSL's column-major registers
  const float offsets[4][3] = {{0, 0, 0}, {0.5f, 0, 0}, {0, 0.25f, 0}, {0, 0, 2}};
  for (int i = 0; i < 4; ++i)
    for (int k = 0; k < 3; ++k)
      constants[(4 + i) * 4 + k] = offsets[i][k];
  int integers[16 * 4] = {};
  BOOL booleans[16] = {};

  VertexCapture capture;
  for (const char *profile : {"vs_1_1", "vs_2_0", "vs_3_0"}) {
    ID3DBlob *code = nullptr, *errors = nullptr;
    if (FAILED(compile(kSource, sizeof(kSource) - 1, "test", nullptr, nullptr, "main", profile, 0, 0, &code,
                       &errors))) {
      std::fprintf(stderr, "%s: %s\n", profile, errors ? static_cast<const char *>(errors->GetBufferPointer()) : "?");
      ++g_failures;
      continue;
    }
    const ShaderCode shader = parseShader(static_cast<const DWORD *>(code->GetBufferPointer()));
    code->Release();
    CHECK(!shader.model3.empty());
    CHECK((shader.outputs & (kOutputColor | kOutputTexcoord)) == (kOutputColor | kOutputTexcoord));
    Draw d;
    d.device = game;
    d.shader = &shader;
    d.elements = elements.data();
    d.declarationHash = 1;
    d.streams[0] = {reinterpret_cast<const uint8_t *>(vertices), sizeof(vertices), 0, sizeof(In)};
    d.constantsF = constants;
    d.constantsI = integers;
    d.constantsB = booleans;
    std::vector<CapturedVertex> out;
    const bool ran = capture.run(d, 0, 4, out);
    CHECK(ran);
    if (!ran)
      continue;
    float worst = 0;
    for (int i = 0; i < 4; ++i) {
      const In &v = vertices[i];
      const float *o = offsets[int(v.index)];
      const float p[4] = {v.x + o[0], v.y + o[1] + std::sin(v.u) * 0.5f, v.z + o[2], 1};
      for (int c = 0; c < 4; ++c) {
        const float expected = p[0] * m[0][c] + p[1] * m[1][c] + p[2] * m[2][c] + p[3] * m[3][c];
        worst = std::max(worst, std::fabs(out[i].clip[c] - expected) / std::max(1.0f, std::fabs(expected)));
      }
      CHECK(std::fabs(out[i].texcoord[0] - v.u * 2) < 1e-3f && std::fabs(out[i].texcoord[1] - v.v * 2) < 1e-3f);
      CHECK(out[i].color == 0xff4080ffu); // RGBA (1, 0.5, 0.25, 1), R in the low byte
    }
    // sincos is approximated in shader model 1 and 2.
    if (worst > 2e-3f)
      std::fprintf(stderr, "%s: clip positions off by %g\n", profile, worst);
    CHECK(worst <= 2e-3f);
  }
  capture.release();
  game->Release();
  d3d->Release();
  DestroyWindow(window);
}

void unprojection() {
  // A world point through world, view and projection comes back.
  D3DMATRIX world = {}, view = {}, projection = {};
  for (int i = 0; i < 4; ++i)
    world.m[i][i] = view.m[i][i] = 1;
  world._41 = 3, world._11 = 2;
  view._43 = 10, view._11 = 0, view._13 = 1, view._31 = -1, view._33 = 0; // a turn about y
  projection._11 = 1.2f, projection._22 = 1.6f, projection._33 = 1.001f, projection._34 = 1, projection._43 = -0.1f;
  const float object[4] = {0.5f, -1, 2, 1};
  float clip[4] = {};
  // object x world x view x projection
  auto multiply = [](const D3DMATRIX &a, const D3DMATRIX &b) {
    D3DMATRIX out = {};
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c)
        for (int k = 0; k < 4; ++k)
          out.m[r][c] += a.m[r][k] * b.m[k][c];
    return out;
  };
  const D3DMATRIX all = multiply(multiply(world, view), projection);
  for (int c = 0; c < 4; ++c)
    clip[c] = object[0] * all.m[0][c] + object[1] * all.m[1][c] + object[2] * all.m[2][c] + all.m[3][c];
  float back[3];
  unproject(makeUnprojection(projection, view, world, true), clip, back);
  CHECK(std::fabs(back[0] - object[0]) < 1e-4f && std::fabs(back[1] - object[1]) < 1e-4f &&
        std::fabs(back[2] - object[2]) < 1e-4f);
  // Without the world matrices: the world point.
  unproject(makeUnprojection(projection, view, world, false), clip, back);
  CHECK(std::fabs(back[0] - (object[0] * 2 + 3)) < 1e-4f);
}

void bridgeChannel() {
  using namespace bridge;
  const std::wstring name = channelName(GetCurrentProcessId()) + L"-test";
  Channel client, host;
  CHECK(client.create(name, 64 * 1024, 1));
  CHECK(host.open(name));
  // Packets larger than the ring stream through it; several threads send.
  constexpr int kThreads = 3, kPackets = 40;
  std::vector<std::thread> senders;
  for (int t = 0; t < kThreads; ++t)
    senders.emplace_back([&, t] {
      for (int i = 0; i < kPackets; ++i) {
        std::vector<uint8_t> data(size_t(1000 + 37 * i + (i == 7 ? 200 * 1024 : 0)), uint8_t(t * 50 + i));
        const uint32_t tag = uint32_t(t << 16 | i);
        client.send(kInput, {{&tag, 4}, {data.data(), data.size()}});
      }
    });
  int received = 0, bad = 0;
  int next[kThreads] = {};
  PacketHeader header;
  std::vector<uint8_t> payload;
  while (received < kThreads * kPackets) {
    if (host.receive(header, payload, 5000) != Channel::Received::Packet) {
      ++bad;
      break;
    }
    uint32_t tag;
    std::memcpy(&tag, payload.data(), 4);
    const int t = int(tag >> 16), i = int(tag & 0xffff);
    const size_t size = size_t(1000 + 37 * i + (i == 7 ? 200 * 1024 : 0));
    // Each sender's packets arrive whole and in its order.
    if (header.command != kInput || t >= kThreads || i != next[t] || payload.size() != 4 + size ||
        payload[4] != uint8_t(t * 50 + i) || payload.back() != uint8_t(t * 50 + i))
      ++bad;
    next[t] = i + 1;
    ++received;
  }
  for (auto &s : senders)
    s.join();
  CHECK(bad == 0 && received == kThreads * kPackets);
  // A gap in the sequence is a desync: the host stops.
  {
    const uint32_t value = 1;
    CHECK(client.send(kSetFullscreen, {{&value, 4}}));
    // Rewrite the sequence of the packet in the ring as if one was lost.
    auto &state = client.state();
    const uint64_t at = (state.written.load() - 4 - sizeof(PacketHeader)) % state.ringSize;
    auto *ring = reinterpret_cast<uint8_t *>(&state) + ((sizeof(SharedState) + 63) & ~size_t(63));
    PacketHeader h;
    std::memcpy(&h, ring + at, sizeof(h));
    h.sequence += 1;
    std::memcpy(ring + at, &h, sizeof(h));
    CHECK(host.receive(header, payload, 1000) == Channel::Received::Broken && host.broken());
  }
}

} // namespace

int main() {
  fvf();
  triangles();
  decoding();
  textures();
  geometry();
  configuration();
  classification();
  textureSelection();
  cameras();
  profilesLayering();
  bridgeChannel();
  unprojection();
  vertexCaptureModels();
  if (g_failures)
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
  else
    std::printf("D3D9 plugin tests passed\n");
  return g_failures ? 1 : 0;
}
