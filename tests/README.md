# Windows graphics probes

## System D3D9On12 probe

Build with `bash tests/build_d3d9on12.sh`. The default output is
`../../dist/d3d9on12-probe/`, with ARM64, ARM64EC and x64 executables.
Run `run_d3d9on12.ps1 -Arch aarch64 -Present` in the interactive desktop
(for example using an interactive scheduled task). Omitting `-Present`
tests rendering and readback only. `-Flip` separately probes D3D9Ex FLIPEX.
The runner records stdout, stderr and exit status and imposes a 45-second
process timeout; this cannot recover a kernel driver hang.

The probe explicitly loads system DXGI/D3D12/D3D9, supplies a VirtIO hardware
D3D12 device to Direct3DCreate9On12Ex, matches the D3D9 adapter by LUID, and
checks the underlying COM device identity and loaded modules. It checks exact
pixels for a clear, a fixed-function triangle and a dynamic texture upload.
Present mode additionally checks 120 PresentEx calls and visible client-area
pixels. Run with an unobscured interactive desktop; the probe temporarily
makes its window topmost. This is a smoke test, not game compatibility coverage.

2026-09-30, Windows ARM64 guest 192.168.1.121:

- 1170: ARM64/ARM64EC/x64 rendering and texture readback passed. ARM64 and
  ARM64EC DISCARD Present crashed after the UMD rejected a source without a
  KMT allocation. ARM64 FLIPEX device creation returned E_INVALIDARG.
- 1171: a lazy runtime-backed shadow surface removed that rejection, but
  kernel Present and the queue worker's CPU fence signal stalled concurrently.
- 1172 after reboot: ARM64, ARM64EC and x64 passed all readback checks, 120 DISCARD presents,
  visible window pixel checks and normal exit 0. Blt copy command lists now
  wait for the queue worker to retire their completion fence before returning
  to the runtime's kernel Present. Native PRIMARY presentation is unchanged.
- The existing native ARM64 D3D12 Hello Texture sample also ran for 8 seconds
  with a live window and exited 0 after CloseMainWindow on 1172. Its UMD module
  path was recorded. This regression checked lifecycle, not screenshot pixels.

Evidence is kept under `../../debug-20260930/d3d9on12-117*/`.
The implementation currently accepts a single-mip, single-sample 2D source
at subresource zero with no explicit destination. It adds one GPU copy and
a CPU completion wait per legacy present. Automatic redirection of unmodified
D3D9 applications, FLIPEX, reset/fullscreen, shared resources and x86 support
are not established by this probe. FLIPEX remains an unresolved creation
failure; the successful path explicitly uses DISCARD. No full suite was run.

## D3D12 fence load probe

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
