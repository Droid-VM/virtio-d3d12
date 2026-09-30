#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${1:-$here/../../dist/d3d9-auto}"
mkdir -p "$out"
if [ -f "$out/d3d9.dll" ]; then
    echo 'Automatic-selection probes must run without an app-local d3d9.dll' >&2
    exit 1
fi
for arch in aarch64 arm64ec x86_64 i686; do
    "${TOOLCHAIN:-/opt/llvm-mingw}/bin/$arch-w64-mingw32-clang++" \
        -std=c++17 -O2 -Wall -Wextra -Werror -static \
        "$here/probe_d3d9_app.cpp" -o "$out/probe_$arch.exe" -ld3d9 -lgdi32
done
cp "$here/run_d3d9_auto.ps1" "$out/"
