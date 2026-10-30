# D3D9 plugin

A drop-in `d3d9.dll` that renders Direct3D 9 games with OpenRemix, 32-bit
and 64-bit alike, whether or not RTX Remix was ever installed. Games set up
for RTX Remix keep their `rtx.conf` and `rtx-remix/mods`: textures and meshes
are keyed by the hashes dxvk-remix computes, and draws are classified by its
rules, as Remix Plus (RemixProjGroup/dxvk-remix) refines them.

It lives in `plugins/d3d9` and is built and packaged on its own; the runtime's
build does not include it.

## Installing

Next to the game's executable, or wherever the game loads `d3d9.dll` from
(Source games: `bin/`):

| Game | Files |
|---|---|
| 64-bit | `x64/`: `d3d9.dll`, `openremix.conf`, `openremix/` (the runtime) |
| 32-bit | `x86/`: `d3d9.dll`, `openremix.conf`, `openremix/` (the runtime and `OpenRemixHost.exe`) |

The plugin reads its settings and the game's RTX Remix files from the folder
of the game's executable. It leaves an RTX Remix install's files (`.trex/`,
`bridge.conf`) alone; only `d3d9.dll` is replaced.

`openremix-d3d9.log` (and, for 32-bit games, `openremix-host.log`) next to the
game records what the plugin found: the window and device, the first frames'
draws by kind, the geometry memo's hits, OpenRemix's own messages. The path
tracer compiles its pipelines on a game's first run (the driver caches them
per executable); it shows "Preparing renderer" and the raster backend
meanwhile.

## How a game is rendered

The plugin loads the system `d3d9.dll`, returns its own objects to the game
and patches their vtables, so the game's device is a real one: locks,
readbacks, queries, state blocks and render-to-texture behave as they always
have. That device renders into a hidden window and never presents; OpenRemix
presents into the game's window. It processes vertices in hardware whatever
the game asked for: Windows' software vertex processing runs vertex shaders on
the CPU, and games made for RTX Remix (HL2 RTX among them) ask for it because
dxvk-remix ignores it. The game is told what it asked for
(`GetCreationParameters`, `Get/SetSoftwareVertexProcessing`), and its buffers
lose `D3DUSAGE_SOFTWAREPROCESSING`.

```
64-bit game: d3d9.dll ── capture ── translator ── DirectRenderer ── openremix/remix.dll
32-bit game: d3d9.dll ── capture ── translator ── RemoteRenderer ══ shared memory ══ OpenRemixHost.exe ── remix.dll
```

### Draws

Each draw is classified in `D3D9Rtx::makeDrawCallType`'s order (`classify.cpp`):
the draw call range, vertex shaders (captured, below), primitive types, alpha
options, occlusion queries, colour writes, Remix Plus's shadow-mask and
stencil-shadow-volume heuristics, render targets of another size than the back
buffer, then UI (an orthographic projection without depth writes, any bound
`rtx.uiTextures`, pre-transformed vertices) and the world. What is not traced
or composited stays the real device's.

- **World**: a path-traced instance. The material comes from the stage
  `processTextures` picks (the used 2D textures up to the first disabled
  stage, binned by texture coordinate set, lightmaps left out); its texture's
  hash keys it. Fixed-function vertex blending (`D3DRS_VERTEXBLEND`, indexed
  or not) becomes the mesh's blend weights and the instance's bone transforms
  (`D3DTS_WORLDMATRIX(n)`). The bones are in the draw's object space and go to
  the world through the draw's view and out through the camera's, as
  dxvk-remix decomposes them. Source's fixed-function path puts a model's
  placement in its view matrix. The plugin keeps all 256 world matrices
  itself: a hardware vertex processing device forgets those past its
  `MaxVertexBlendMatrixIndex`. The enabled lights become sphere, spot and
  distant lights by dxvk-remix's conversion and hash whenever the light state
  changes; fog is the first Main draw's.
- **Cameras** (Remix Plus's camera rules): a draw's matrices make it a Main,
  view model (`rtx.viewModel.enable`: another field of view, or a viewport up
  to `rtx.viewModel.maxZThreshold`) or sky camera's, or no camera's (identity
  or sheared projections). Sky draws (`rtx.skyBoxTextures`, a viewport from
  `rtx.skyMinZThreshold`, untextured draws before `rtx.skyDrawcallIdThreshold`)
  and view models are instances of those categories with their cameras.
- **UI**: composited over the frame, in draw order, with its blend factors.
- Instances are double-sided: the raster backend and the path tracer disagree
  on a left-handed world's front faces.

### Vertex capture

A draw with a vertex shader (`rtx.useVertexCapture`, on by default as in
dxvk-remix) is captured: its shader runs over the draw's vertices on
Direct3D's own software vertex processor (`ProcessVertices` on a private
device of the plugin, `vertex_capture.cpp`), with the game's constants, and
each clip-space position comes back to object space through the draw's
fixed-function transforms, as Remix Plus's `emitVertexCaptureOp` takes it:
`(clip x inverse(D3DTS_PROJECTION)) / w x inverse(D3DTS_VIEW) x
inverse(D3DTS_WORLD)`. Games made for RTX Remix keep those transforms set for
this; `rtx.useWorldMatricesForShaders` off leaves positions in world space.

- The software vertex processor runs only vs_3_0 shaders in
  `ProcessVertices`: vs_1_x and vs_2_x bytecode is translated to vs_3_0
  (outputs declared as `o#`, vs_1_x's `a0` moves as `mova` and its relative
  addressing given its register, vs_2_x's `sincos` without its helper
  constants, instruction lengths set). Shaders that sample textures (`texldl`)
  are not captured.
- Texture coordinates come from the shader with
  `rtx.useVertexCapturedTexcoords` (or when the vertices have none), colours
  when no pixel shader is bound, normals from the vertices
  (`rtx.useVertexCapturedNormals`). The asset hash is the input positions',
  as in dxvk-remix.
- A captured draw is memoized on its shader, its constants and its
  transforms besides its buffers; an animated one (its constants change)
  is captured again each frame, its previous mesh replaced at once.

### Hashes

As dxvk-remix computes them (`hash.cpp`):

- Geometry: `rtx.geometryAssetHashRuleString` (default positions, indices,
  geometry descriptor; also texcoords, vertex layout), components chained with
  XXH64 in dxvk-remix's order. Positions are hashed over the distinct indices,
  or every vertex of a non-indexed draw (whose indices hash 0 and descriptor
  index count and type 0). A draw whose indices are all equal is dropped.
- Mesh replacement: the asset hash XOR the material texture's hash.
- Textures: XXH3 of mip 0 at a 4-byte aligned pitch, taken when a draw first
  uses the texture after it was written and kept
  (`rtx.recomputeTextureHashOnWrite` retakes it), XXH64 for lock uploads with
  `rtx.useObsoleteHashOnTextureUpload`; `UpdateTexture` and whole
  `UpdateSurface` copies take the source's hash.

A draw whose material texture has no hash is left to the real device, as
dxvk-remix leaves it.

### Geometry memoization

Every vertex and index buffer has a version that each write bumps. A draw of
the same buffers at the same versions, offsets, strides and ranges (Remix
Plus's `buildGeometryHashMemoizationKey`) reuses the decoded vertices and the
asset hash: an unchanged mesh costs a key per frame. At most 16384 entries;
UP draws are decoded every time.

## 32-bit games: OpenRemixHost.exe

OpenRemix is 64-bit. A 32-bit game runs the same plugin in its own process,
with its own real 32-bit device, and the plugin's renderer calls cross to
`openremix/OpenRemixHost.exe`, a 64-bit process the plugin starts, which makes
them on OpenRemix. The bridge carries the plugin's output, not D3D9: textures
(once, as they are filled), meshes (once per key), instances, lights, cameras,
the UI list and Present, every one fire-and-forget. The game never waits on the
host except for a frame slot at Present (`openremix.bridgeFramesAhead`, default
1), and every D3D9 call the game makes is answered by its own device.

- Transport: one named shared-memory ring (`openremix.bridgeMemory` MB,
  default 64), any packet size streamed through it, a sequence number per
  packet (the host stops on a gap), spin-then-block waits that also watch the
  other process (`bridge/channel.cpp`).
- The host answers through a shared status block: started or failed, the
  active backend, whether OpenRemix's menu wants keyboard or mouse.
- Input: the plugin subclasses the game window and forwards keyboard, mouse
  and focus messages to the host, which hands them to OpenRemix's window as
  SDL events (`bridge/host_input.cpp`; SDL gives keyboard focus only to a
  foreground window, which OpenRemix's stand-in never is). WM_INPUT is never
  forwarded.
- The game's window belongs to another process: OpenRemix presents into it
  through a Vulkan surface on the handle without messaging it
  (`platform/window.cpp`).
- If the host exits or fails, the plugin stops sending and the game's own
  device presents; the host exits with the game.

## Input while OpenRemix's menu is open

The game sees no keyboard or mouse (both architectures, `input_hooks.cpp`, as
Remix Plus's bridge neutralizes them): its window messages are swallowed, and
the user32 functions it polls are patched in every module's imports. Cursor
position reads return where the game last put the cursor and its
re-centring does not move it (mouse-look games turn no more), raw mouse input
carries no movement or buttons, and key state reads are empty.

## Configuration

`dxvk.conf`, the game's built-in profile, `user.conf`, `rtx.conf` (later
wins; `DXVK_CONFIG_FILE` and `DXVK_RTX_CONFIG_FILE` honoured) and then
`openremix.conf`, next to the game's executable. The profiles are dxvk-remix's
application defaults among the options the plugin reads (`profiles.cpp`,
generated by `scripts/gen_profiles.py` from Remix Plus's
`src/util/config/config.cpp`): for example `hl2.exe`'s texture lists,
`rtx.useObsoleteHashOnTextureUpload`, `rtx.zUp` and view models.

| Key | Effect |
|---|---|
| `rtx.uiTextures` and the other `rtx.*Textures` lists, `rtx.lightmapTextures` | UI, instance categories, the material stage |
| `rtx.orthographicIsUI`, `rtx.preTransformedVerticesIsUI` | UI detection |
| `rtx.enableAlphaBlend`, `rtx.enableAlphaTest`, `rtx.drawCallRange` | draws not traced |
| `rtx.geometryAssetHashRuleString`, `rtx.useObsoleteHashOnTextureUpload`, `rtx.recomputeTextureHashOnWrite`, `rtx.allowCubemaps` | hashing |
| `rtx.useVertexCapture`, `rtx.useVertexCapturedTexcoords`, `rtx.useVertexCapturedNormals`, `rtx.useWorldMatricesForShaders` | vertex capture |
| `rtx.viewModel.enable`, `rtx.viewModel.maxZThreshold`, `rtx.skyMinZThreshold`, `rtx.skyDrawcallIdThreshold`, `rtx.fusedWorldViewMode` | cameras and sky |
| `rtx.zUp`, `rtx.sceneScale` | OpenRemix's up axis and scene scale (metres per unit: 0.01 / `rtx.sceneScale`) |
| `rtx.ignoreLights` | lights not converted (dxvk-remix's light hash) |
| other `rtx.*` | passed to OpenRemix (`SetConfigVariable`); it applies the ones it knows |
| `openremix.backend` | `raster` or `pathtrace` (default: the runtime's) |
| `openremix.mods` | load `rtx-remix/mods` (default on) |
| `openremix.forwardDraws` | also rasterize captured draws on the real device |
| `openremix.bridgeMemory`, `openremix.bridgeFramesAhead` | 32-bit games' bridge |
| `openremix.log` | write the logs (default on); every 2 s they record the frame rate and where a frame's time went (the host's too) |
| `openremix.hashLog` | a file listing the texture hashes, mesh replacement keys and light hashes the game's draws had, rewritten every 600 frames, to check against a mod's `mat_`, `mesh_` and `light_` names |
| `openremix.drawLog`, `openremix.drawLogFrame` | a file describing every draw of one frame (default 600): its classification and why, its vertex blending, shader, matrices and viewport, and where a skinned draw's first vertex lands |
| `openremix.screenshot` with `openremix.screenshotFrame` or `openremix.screenshotAfterActive` | save the presented frame at that frame, or that many frames after the backend became active |

## Building

```
plugins/d3d9/scripts/build.ps1    # x64: d3d9.dll, OpenRemixHost.exe, tests, sample; x86: d3d9.dll, tests, sample
python plugins/d3d9/scripts/package.py --openremix <runtime package bin> [--remix-dll <remix.dll>]
```

Both architectures build with Visual Studio's clang-cl (`-m32` for x86) and
use the runtime's public headers and `third_party/xxhash`. Packages go to
`_work/packages/d3d9-plugin/{x64,x86}`.

`samples/ff-cube` is a D3D9 app (a textured cube lit by a point light, a floor,
a pyramid drawn without indices, a column bent by vertex blending, a sheet a
vs_2_0 shader waves, a view-model box, a pre-transformed HUD bar;
`--frames N`), built
for x64 and x86: run it from a package folder to see the plugin in process or
through the host. `tests/tests.cpp` covers vertex decoding, triangulation,
the hashes (against their definition), texture conversion, configuration and
profiles, texture stage selection, draw classification, the camera and sky
rules, the bridge's channel, unprojection, and vertex capture of one shader
compiled for vs_1_1, vs_2_0 and vs_3_0 (with a D3D9 device and
`d3dcompiler_47.dll`).

## Not yet

- Vertex-shader capture needs the game's fixed-function transforms
  (as dxvk-remix's does); shaders that sample textures, and UI drawn with
  vertex shaders, are left to the real device.
- Render targets the scene samples (mirrors, monitors), cube and volume
  texture contents, texture transforms, a second material texture.
- Sky auto-detection (`rtx.skyAutoDetect`) and the sky's reprojection into the
  main camera; `rtx.fusedWorldViewMode` only in the camera rules.
