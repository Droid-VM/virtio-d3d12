# Microsoft DX12 sample bundles (ARM64)

These build adapters produce Hello Texture and ExecuteIndirect from Microsoft's
MIT-licensed DirectX-Graphics-Samples. They use the system D3D12 runtime instead
of the samples' Agility 618 exports. Rendering and HLSL remain unchanged; PIX
markers are disabled and the patches adapt the sources to LLVM-MinGW.

Pinned source revisions are recorded in
`win3d/dist/dx12-app-validation-1167/README.md` (relative to the workspace).
The sample source revision is `e5975f9b0744fc5096dd593ed72bf8f5c004164f`.
DXC v1.8.2505.1 was used for the Shader Model 6.0 bytecode, matching the profiles
and entry points in the upstream vcxproj files.

Run from the workspace root:

```sh
python3 win3d/virtio-d3d12/tests/microsoft-samples/build.py \
  --samples "$HOME/.cache/DirectX-Graphics-Samples" \
  --headers "$HOME/.cache/DirectX-Headers" \
  --math "$HOME/.cache/DirectXMath" \
  --dxc "$HOME/.cache/dxc-linux/bin/dxc" \
  --output "$HOME/.cache/dx12-complete-1167"
```

Deploy each complete output directory. Both executables require
`shaders_VSMain.cso` and `shaders_PSMain.cso` beside the exe; ExecuteIndirect also
requires `compute.cso`. HLSL source alone is insufficient: the upstream app
reads precompiled bytecode and throws if a file is missing. An uncaught exception
can terminate through ucrtbase with 0xc0000409 / FAST_FAIL_FATAL_APP_EXIT (7).
The build stages all shader and executable outputs before publishing bundles.
