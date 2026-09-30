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

## DXGI display modes and missing saved configuration

Build `probe_dxgi_modes.cpp` with LLVM-MinGW (use `x86_64` for the emulated x64
comparison):

```sh
aarch64-w64-mingw32-clang++ -O2 -static probe_dxgi_modes.cpp -ldxgi -o probe_dxgi_modes.exe
```

Run in the logged-in interactive desktop, not an SSH service session. Without
arguments it checks current/saved GDI settings and both count/list calls for
DXGI RGBA8/BGRA8 modes with flags 0..3. An empty list with S_OK is reported as
an unsupported format, not an API error; at least one nonempty list is required.

If current settings exist but ENUM_REGISTRY_SETTINGS fails, the opt-in
`--repair-missing` validates the active topology and saves that same topology
with SetDisplayConfig(SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
SDC_SAVE_TO_DATABASE). It does not select a new resolution or overwrite an
existing saved mode. Use an elevated interactive session. It never writes
registry values directly and returns failure if any display API call fails.

On 1167, missing saved display configuration caused GetDisplayModeList to return
DXGI_ERROR_NOT_CURRENTLY_AVAILABLE (0x887A0022), despite KMT/GDI enumerating 18
modes. ChangeDisplaySettingsEx(CDS_UPDATEREGISTRY | CDS_NORESET) failed even when
elevated; saving the active configuration with SetDisplayConfig succeeded.
Afterward ARM64 and x64 DXGI calls returned S_OK, with 18 BGRA8 modes and zero
RGBA8 modes. This repairs the availability error, not RGBA8 mode support, and
is not proof that the full Night Raid benchmark completes. The event that
originally lost the saved configuration has not been established.
