#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
win3d="$(cd "$here/../.." && pwd)"
export PATH="${TOOLCHAIN:-/opt/llvm-mingw}/bin:$PATH"
export PREFIX=i686-w64-mingw32
export JOBS="${JOBS:-8}"
src="$win3d/vkd3d-proton"
build="$src/build.umd-x86"
if [ ! -f "$build/build.ninja" ]; then
    meson setup "$build" "$src" --cross-file "$here/vkd3d.ini" \
        --buildtype release -Denable_umd=true -Denable_tests=false \
        -Denable_extras=false -Denable_trace=false
fi
ninja -C "$build" -j "$JOBS" \
    libs/vkd3d/libvkd3d-proton.a libs/vkd3d-shader/libvkd3d-shader.a \
    libs/vkd3d-common/libvkd3d_common.a subprojects/dxil-spirv/libdxil-spirv.a \
    subprojects/dxil-spirv/libdxbc_spv_module.a \
    subprojects/dxil-spirv/subprojects/dxbc-spirv/libdxbc_spv.a
make -C "$here/.." -j "$JOBS" PREFIX="$PREFIX" BUILD_DIR=build-x86 VKD3D_BUILD="$build"
BUILD_DIR="$win3d/../mesa/build-win-x86" DIST_DIR="$win3d/dist/turnip-x86" \
    CROSS_FILE="$here/mesa.ini" bash "$win3d/build_turnip.sh"
loader="$win3d/build-vulkan-loader-$PREFIX"
cmake -S "$win3d/Vulkan-Loader" -B "$loader" -G Ninja \
    -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=x86 \
    -DCMAKE_C_COMPILER="$PREFIX-clang" -DCMAKE_CXX_COMPILER="$PREFIX-clang++" \
    -DCMAKE_RC_COMPILER="$PREFIX-windres" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTS=OFF -DBUILD_WERROR=OFF -DUSE_MASM=OFF -DUSE_GAS=OFF \
    -DVULKAN_HEADERS_INSTALL_DIR="$win3d/dist/vulkan-headers"
cmake --build "$loader" -j "$JOBS"
out="$win3d/dist/d3d12-x86"
mkdir -p "$out"
cp "$here/../build-x86/dist/dx12um_virtio.dll" "$out/dx12um_virtio_x86.dll"
cp "$win3d/dist/turnip-x86/vulkan_freedreno.dll" "$out/vulkan_freedreno_x86.dll"
cp "$loader/loader/vulkan-1.dll" "$out/droidvm_vulkan_x86.dll"
