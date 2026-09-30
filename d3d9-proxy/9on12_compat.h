#pragma once
#include <d3d9.h>
#include <d3d12.h>

// Public Windows SDK d3d9on12.h ABI, absent from the current LLVM-MinGW headers.
struct D3D9ON12_ARGS {
    BOOL Enable9On12;
    IUnknown *pD3D12Device;
    IUnknown *ppD3D12Queues[2];
    UINT NumQueues, NodeMask;
};
using Create9On12 = IDirect3D9 *(WINAPI *)(UINT,D3D9ON12_ARGS *,UINT);
using Create9On12Ex = HRESULT (WINAPI *)(UINT,D3D9ON12_ARGS *,UINT,IDirect3D9Ex **);
static const GUID iid_device9on12 = {0xe7fda234,0xb589,0x4049,{0x94,0x0d,0x88,0x78,0x97,0x75,0x31,0xc8}};
