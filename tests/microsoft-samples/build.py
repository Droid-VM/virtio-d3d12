#!/usr/bin/env python3
"""Build complete ARM64 Microsoft DX12 sample bundles, including SM6 shaders."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--samples', type=Path, required=True)
parser.add_argument('--headers', type=Path, required=True)
parser.add_argument('--math', type=Path, required=True)
parser.add_argument('--dxc', required=True)
parser.add_argument('--cxx', default='/opt/llvm-mingw/bin/aarch64-w64-mingw32-clang++')
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output = args.output.resolve()
args.output.mkdir(parents=True, exist_ok=True)
paths = {
    'HelloTexture': 'Samples/Desktop/D3D12HelloWorld/src/HelloTexture',
    'ExecuteIndirect': 'Samples/Desktop/D3D12ExecuteIndirect/src',
}
# Build in a staging directory: never publish an exe with missing shaders.
with tempfile.TemporaryDirectory(prefix='dx12-samples-') as tmp:
    stage = Path(tmp)
    for name, source in paths.items():
        src = stage / source
        shutil.copytree(args.samples / source, src)
        for path in src.iterdir():
            if path.suffix in ('.cpp', '.h'):
                path.write_text(path.read_text(encoding='utf-8-sig'))
        subprocess.run(['patch', '--batch', '-p1', '-i', str(
            Path(__file__).resolve().parent / (name + '-mingw.patch'))],
            cwd=stage, check=True)
        bundle = stage / name
        bundle.mkdir()
        shaders = [('shaders.hlsl', 'VSMain', 'vs_6_0', 'shaders_VSMain.cso'),
                   ('shaders.hlsl', 'PSMain', 'ps_6_0', 'shaders_PSMain.cso')]
        if name == 'ExecuteIndirect':
            shaders.append(('compute.hlsl', 'CSMain', 'cs_6_0', 'compute.cso'))
        for filename, entry, profile, output in shaders:
            subprocess.run([args.dxc, '-nologo', '-T', profile, '-E', entry,
                            '-Zi', '-Qembed_debug', '-Fo', str(bundle / output),
                            str(src / filename)], check=True)
        subprocess.run([args.cxx, '-std=c++17', '-O2', '-DUNICODE', '-D_UNICODE',
                        '-DNOMINMAX', '-DD3DX12_NO_STATE_OBJECT_HELPERS',
                        '-DD3DX12_NO_CHECK_FEATURE_SUPPORT_CLASS',
                        '-Wno-address-of-temporary',
                        '-I' + str(args.headers.resolve() / 'include/directx'),
                        '-I' + str(args.math.resolve() / 'Inc'),
                        *map(str, sorted(src.glob('*.cpp'))),
                        '-o', str(bundle / (name + '.exe')), '-mwindows', '-static',
                        '-ld3d12', '-ldxgi', '-ld3dcompiler', '-ldxguid'], check=True)
        for path in src.glob('*.hlsl'):
            shutil.copy2(path, bundle / path.name)
        for path in bundle.iterdir():
            if not path.stat().st_size:
                raise RuntimeError('Empty build output: ' + str(path))
    for name in paths:
        shutil.copytree(stage / name, args.output / name, dirs_exist_ok=True)
shutil.copy2(args.samples / 'LICENSE', args.output / 'LICENSE')
manifest = []
for name in paths:
    for path in sorted((args.output / name).iterdir()):
        if path.is_file():
            manifest.append(hashlib.sha256(path.read_bytes()).hexdigest() +
                            '  ' + str(path.relative_to(args.output)) + '\n')
(args.output / 'SHA256SUMS').write_text(''.join(manifest))
print('Complete sample bundles:', args.output)
