#!/usr/bin/env python3
"""Package the D3D9 plugin as drop-in folders for Direct3D 9 games.

x64/: for 64-bit games: d3d9.dll, openremix.conf and openremix/ (the runtime)
      go next to the game's executable (or wherever it loads d3d9.dll from).
x86/: for 32-bit games: the same, with the 32-bit d3d9.dll and
      openremix/OpenRemixHost.exe, which runs OpenRemix (64-bit) for the game.
Neither touches an RTX Remix install's files (.trex/, bridge.conf).
"""
import argparse
from pathlib import Path
import shutil

root = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--openremix', type=Path, required=True,
                    help='folder with remix.dll and its runtime libraries (an OpenRemix package bin/)')
parser.add_argument('--remix-dll', type=Path,
                    help='a remix.dll to use instead of the one in --openremix (e.g. a fresh runtime build)')
parser.add_argument('--plugin', type=Path, default=root / '_work/build/d3d9-plugin',
                    help='scripts/build.ps1 output (x64); the x86 build is <plugin>-x86')
parser.add_argument('--output', type=Path, default=root / '_work/packages/d3d9-plugin')
args = parser.parse_args()
output = args.output.resolve()
if output.exists():
    parser.error(f'Use a fresh output directory ({output} exists).')

plugin64 = args.plugin / 'd3d9.dll'
plugin32 = args.plugin.parent / f'{args.plugin.name}-x86' / 'd3d9.dll'
host = args.plugin / 'OpenRemixHost.exe'
runtime = args.openremix
for required in (plugin64, host):
    if not required.exists():
        parser.error(f'{required} not found: run plugins/d3d9/scripts/build.ps1')
if not (runtime / 'remix.dll').exists():
    parser.error(f'{runtime / "remix.dll"} not found')

CONFIG = """\
# OpenRemix D3D9 plugin settings (next to the game's executable).
# The game's own rtx.conf, dxvk.conf and user.conf are read as RTX Remix reads
# them; these keys are the plugin's own. See docs/features/d3d9-plugin.md.
# openremix.backend = pathtrace
# openremix.mods = True
# openremix.forwardDraws = False
# openremix.log = True
"""


def runtime_files(destination: Path, with_host: bool):
    # The runtime and its libraries; not samples, caches or saved profiles.
    shutil.copytree(runtime, destination,
                    ignore=shutil.ignore_patterns('*.exe', '*.pdb', 'cache', '*.request', 'profiles*'))
    if args.remix_dll:
        shutil.copy2(args.remix_dll, destination / 'remix.dll')
    if with_host:
        shutil.copy2(host, destination / 'OpenRemixHost.exe')
    licenses = destination / 'licenses'
    licenses.mkdir(exist_ok=True)
    shutil.copy2(root / 'LICENSE', licenses / 'OpenRemix-LICENSE.txt')
    shutil.copy2(root / 'ThirdPartyLicenses.txt', licenses / 'ThirdPartyLicenses.txt')
    shutil.copy2(root / 'third_party/xxhash/LICENSE', licenses / 'xxHash.txt')


def package(name: str, dll: Path, with_host: bool):
    folder = output / name
    folder.mkdir(parents=True)
    shutil.copy2(dll, folder / 'd3d9.dll')
    (folder / 'openremix.conf').write_text(CONFIG)
    runtime_files(folder / 'openremix', with_host)
    print(f'{name} package: {folder}')


package('x64', plugin64, False)
if plugin32.exists():
    package('x86', plugin32, True)
else:
    print(f'No x86 package: {plugin32} is not built (scripts/build.ps1 builds it)')
