# Builds the D3D9 plugin with clang-cl: x64 (the 64-bit d3d9.dll,
# OpenRemixHost.exe, tests, sample) and x86 (the 32-bit d3d9.dll, tests,
# sample). Needs Visual Studio's MSVC x86/x64 tools and Clang tools, and Meson.
param([string]$Build = "_work/build/d3d9-plugin", [switch]$NoTests)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot/../../..").Path
$source = Join-Path $root "plugins/d3d9"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the MSVC x86/x64 tools not found" }
$vcvars = Join-Path $vs "VC/Auxiliary/Build/vcvarsall.bat"
$llvm = Join-Path $vs "VC/Tools/Llvm/x64/bin"
# vcvarsall finds the installation with vswhere on PATH; the tools write
# progress to stderr, which is not an error.
$env:PATH = "$(Split-Path $vswhere);$env:PATH"
$ErrorActionPreference = "Continue"

function Build([string]$arch, [string]$compiler, [string]$dir, [bool]$test) {
  $setup = if (Test-Path "$dir/meson-private/coredata.dat") { "--reconfigure" } else { "" }
  # Visual Studio's clang-cl (another LLVM on PATH may be too old for its STL).
  $commands = "call `"$vcvars`" $arch >nul && set `"PATH=$llvm;%PATH%`" && set CXX=$compiler&& meson setup $setup `"$dir`" `"$source`" && meson compile -C `"$dir`""
  if ($test) { $commands += " && meson test -C `"$dir`" --print-errorlogs" }
  cmd /c $commands
  if ($LASTEXITCODE) { exit $LASTEXITCODE }
}

Build "x64" "clang-cl" (Join-Path $root $Build) (-not $NoTests)
Build "x86" "clang-cl -m32" (Join-Path $root "$Build-x86") (-not $NoTests)
Write-Host "Plugin built: $Build (d3d9.dll, OpenRemixHost.exe), $Build-x86 (d3d9.dll)"
