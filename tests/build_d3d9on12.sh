#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${1:-$here/../../dist/d3d9on12-probe}"
mkdir -p "$out"
for target in aarch64 arm64ec x86_64; do
    "${TOOLCHAIN:-/opt/llvm-mingw}/bin/$target-w64-mingw32-clang++" \
        -std=c++17 -O2 -Wall -Wextra -Werror -static \
        "$here/probe_d3d9on12.cpp" -o "$out/probe_d3d9on12_$target.exe" -ldxguid -luuid -lgdi32
done
cp "$here/run_d3d9on12.ps1" "$out/"
