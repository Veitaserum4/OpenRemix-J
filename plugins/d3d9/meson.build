# OpenRemix D3D9 plugin: a drop-in d3d9.dll for Direct3D 9 games
# (docs/features/d3d9-plugin.md). Built separately from the runtime, with
# scripts/build.ps1: once for x64 (the 64-bit plugin, OpenRemixHost.exe, the
# tests) and once for x86 (the 32-bit plugin).
project('openremix-d3d9', 'cpp', version: '0.2.0',
        default_options: ['cpp_std=c++20', 'warning_level=2', 'buildtype=release', 'b_ndebug=if-release',
                          'b_vscrt=static_from_buildtype'])

if host_machine.system() != 'windows'
  error('the D3D9 plugin is Windows-only')
endif

add_project_arguments('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', language: 'cpp')

fs = import('fs')
repository = fs.parent(fs.parent(meson.current_source_dir()))
includes = include_directories(repository / 'public' / 'include', repository / 'third_party' / 'xxhash', 'src')
# The pointer size, not host_machine: clang-cl -m32 in an x86 Visual Studio
# environment still reports an x86_64 host.
x64 = meson.get_compiler('cpp').sizeof('void*') == 8
# The 32-bit plugin uses the Remix API's types only (OpenRemixHost.exe calls it).
if not x64
  add_project_arguments('-DREMIX_ALLOW_X86', language: 'cpp')
endif

# Engine-independent parts, shared with the tests and the host.
core = static_library('openremix-d3d9-core',
  'src/camera.cpp', 'src/classify.cpp', 'src/config.cpp', 'src/formats.cpp', 'src/hash.cpp', 'src/log.cpp',
  'src/profiles.cpp', 'src/vertex.cpp', 'src/vertex_capture.cpp',
  'src/bridge/channel.cpp',
  include_directories: includes,
)

# The plugin: OpenRemix in process on x64; on x86, the bridge to
# OpenRemixHost.exe (OpenRemix is 64-bit).
shared_library('d3d9',
  'src/hooks.cpp', 'src/input_hooks.cpp', 'src/main.cpp', 'src/translator.cpp',
  x64 ? 'src/direct_renderer.cpp' : 'src/bridge/client.cpp',
  include_directories: includes,
  link_with: core,
  name_prefix: '',
  vs_module_defs: 'd3d9.def',
  link_args: ['user32.lib'],
)

if x64
  # SDL's headers: the host hands OpenRemix's window the game's input as SDL
  # events (SDL3.dll is the runtime's).
  sdl_includes = include_directories(repository / 'subprojects' / 'SDL3-3.4.14' / 'include')
  executable('OpenRemixHost', 'src/bridge/host.cpp', 'src/bridge/host_input.cpp', 'src/direct_renderer.cpp',
    include_directories: [includes, sdl_includes],
    link_with: core,
    cpp_args: ['-DOPENREMIX_D3D9_HOST'],
    link_args: ['user32.lib', '/ENTRY:wmainCRTStartup'],
    win_subsystem: 'windows',
  )
endif

# The tests load the system's d3d9.dll by its path: the plugin built next to
# them would be found first.
tests = executable('openremix-d3d9-tests', 'tests/tests.cpp',
  include_directories: includes,
  link_with: core,
  link_args: ['user32.lib'],
)
test('d3d9-plugin', tests)

# A fixed-function D3D9 app (textured, lit cube on a floor, and a
# pre-transformed HUD bar) to run the plugin against.
executable('ff-cube', 'samples/ff-cube/main.cpp',
  link_args: ['d3d9.lib', 'user32.lib'],
  win_subsystem: 'windows',
)
