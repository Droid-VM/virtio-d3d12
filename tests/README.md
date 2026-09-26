# D3D12 fence load probe

Build for each Windows guest architecture with llvm-mingw:

```sh
/opt/llvm-mingw/bin/aarch64-w64-mingw32-clang -std=gnu23 -O2 -Wall -Wextra \
    -o probe_d3d12_fence_load_arm64.exe tests/probe_d3d12_fence_load.c \
    -ld3d12 -ldxgi -ldxguid
/opt/llvm-mingw/bin/arm64ec-w64-mingw32-clang -std=gnu23 -O2 -Wall -Wextra \
    -o probe_d3d12_fence_load_arm64ec.exe tests/probe_d3d12_fence_load.c \
    -ld3d12 -ldxgi -ldxguid
```

Run outside a directory containing app-local D3D12 DLLs. The probe selects the
virtio adapter and uses the system D3D12 runtime. Each row reports the fence
value immediately after submission and the time until `GetCompletedValue`
reaches the target. The larger copy counts must take longer, and the immediate
value should remain at the previous submission's value.
