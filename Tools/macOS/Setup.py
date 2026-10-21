#!/usr/bin/env python3
"""Validate native macOS tools and save process-local build settings."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def command_output(arguments, cwd=None):
    return subprocess.check_output(list(map(str, arguments)), cwd=cwd, text=True, stderr=subprocess.STDOUT).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dotnet-root', type=Path)
    parser.add_argument('--dxc', type=Path)
    parser.add_argument('--moltenvk-root', type=Path, default=Path('/opt/homebrew/opt/molten-vk'))
    parser.add_argument('--libclang-root', type=Path, default=Path('/opt/homebrew/opt/llvm'))
    parser.add_argument('--python', type=Path, default=Path('/opt/homebrew/bin/python3.11'))
    parser.add_argument('--install-node-tools', action='store_true', help='Run the repository-locked npm ci; no global packages')
    parser.add_argument('--check-only', action='store_true', help='Check without changing saved settings')
    args = parser.parse_args()
    if sys.platform != 'darwin' or os.uname().machine != 'arm64':
        parser.error('The native macOS toolchain requires Apple Silicon')
    repo = Path(__file__).resolve().parents[2]
    dependencies = repo / 'out/agent/dependencies/macos'
    dotnet = (args.dotnet_root or dependencies / 'dotnet').resolve()
    dxc = args.dxc
    if not dxc:
        candidates = sorted((dependencies / 'VulkanSDK').glob('*/macOS/bin/dxc'))
        if candidates:
            dxc = candidates[-1]
    if not dxc:
        parser.error('Supply --dxc with the DXC executable from the macOS Vulkan SDK')
    for name in ('cmake', 'ninja', 'pwsh', 'node', 'xcrun', 'codesign', 'otool', 'install_name_tool'):
        if not shutil.which(name):
            raise RuntimeError('Missing host tool: ' + name)
    checks = {'clang': command_output(['xcrun', '--find', 'clang']),
              'dotnetSdk': command_output([dotnet / 'dotnet', '--version'], repo / 'Engine/Managed'),
              'python': command_output([args.python, '-c', 'import sys; print(".".join(map(str,sys.version_info[:3])))']),
              'dxc': command_output([dxc, '--version']),
              'cmake': command_output(['cmake', '--version']).splitlines()[0]}
    if checks['dotnetSdk'] != '10.0.302':
        raise RuntimeError('The project requires the pinned .NET SDK 10.0.302')
    if not checks['python'].startswith('3.11.'):
        raise RuntimeError('The embedded Python plugin requires Python 3.11')
    moltenvk = args.moltenvk_root.resolve()
    libclang = args.libclang_root.resolve()
    if not (moltenvk / 'lib/libMoltenVK.dylib').is_file() or not (moltenvk / 'LICENSE').is_file():
        raise RuntimeError('MoltenVK requires lib/libMoltenVK.dylib and LICENSE')
    if 'arm64' not in command_output(['lipo', '-archs', moltenvk / 'lib/libMoltenVK.dylib']).split():
        raise RuntimeError('MoltenVK does not contain arm64 code')
    if not (libclang / 'lib/libclang.dylib').is_file():
        raise RuntimeError('libclang.dylib is missing from the LLVM installation')
    if args.install_node_tools:
        if args.check_only:
            parser.error('--check-only cannot install Node tools')
        subprocess.run(['npm', 'ci', '--ignore-scripts', '--no-audit', '--no-fund'], cwd=repo / 'Tools/Scripting', check=True)
    checks['esbuild'] = command_output(['node', '-e', 'const e=require("./node_modules/esbuild");console.log(e.version);e.transformSync("let x=1");'], repo / 'Tools/Scripting')
    if checks['esbuild'] != '0.25.12':
        raise RuntimeError('Run Setup.py --install-node-tools to prepare the locked esbuild')
    settings = {'schema': 1, 'dotnetRoot': str(dotnet), 'dxc': str(dxc.resolve()),
                'moltenvkRoot': str(moltenvk), 'libclangRoot': str(libclang),
                'python': str(args.python.resolve()),
                'pythonRoot': command_output([args.python, '-c', 'import sys; print(sys.base_prefix)']), 'checks': checks}
    if not args.check_only:
        dependencies.mkdir(parents=True, exist_ok=True)
        destination = dependencies / 'settings.json'
        temporary = destination.with_suffix('.tmp')
        temporary.write_text(json.dumps(settings, indent=2) + '\n')
        temporary.replace(destination)
    print(json.dumps({'status': 'PASS', **settings}, indent=2))


if __name__ == '__main__':
    main()
