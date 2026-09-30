#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${1:-$here/../../dist/d3d9-proxy}"
for arch in aarch64 x86_64 i686; do
    mkdir -p "$out/$arch"
    python3 "$here/generate_forwarders.py" "$arch" "$out/$arch"
    "${TOOLCHAIN:-/opt/llvm-mingw}/bin/$arch-w64-mingw32-clang++" \
        -std=c++17 -O2 -Wall -Wextra -Werror -static -shared \
        -I"$out/$arch" "$here/proxy.cpp" "$out/$arch/forwarders.S" "$out/$arch/d3d9.def" \
        -o "$out/$arch/d3d9.dll"
done
for arch in aarch64 x86_64 arm64ec i686; do
    mkdir -p "$out/$arch"
    "${TOOLCHAIN:-/opt/llvm-mingw}/bin/$arch-w64-mingw32-clang++" \
        -std=c++17 -O2 -Wall -Wextra -Werror -static \
        "$here/../tests/probe_d3d9_app.cpp" -o "$out/$arch/probe_d3d9_app.exe" -ld3d9 -lgdi32
done
# ARM64EC processes can load x64 DLLs through Windows' supported interop.
# Only the factory/export forwarding executes emulated code; rendering uses
# the system ARM64EC runtime and native ARM64EC UMD.
cp "$out/x86_64/d3d9.dll" "$out/arm64ec/d3d9.dll"
cp "$here/run-tests.ps1" "$out/"
cp "$here/install.ps1" "$here/README.md" "$out/"
