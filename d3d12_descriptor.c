#include "d3d12_umd.h"

/* ---------------------------------------------------------------------------
 * Descriptor heaps (dx12plan.md §2.3). The driver picks descriptor sizes and
 * handle values, so vkd3d's own are handed through: runtime and application
 * arithmetic on them lands on vkd3d's.
 */

static_assert(sizeof(D3D12DDI_CPU_DESCRIPTOR_HANDLE) == sizeof(D3D12_CPU_DESCRIPTOR_HANDLE));
static_assert(sizeof(D3D12DDI_GPU_DESCRIPTOR_HANDLE) == sizeof(D3D12_GPU_DESCRIPTOR_HANDLE));

/* Every DDI heap needs a GPU handle, shader-visible or not; vkd3d gives the
 * latter none. Such handles never reach a command list, so any value that
 * cannot collide with vkd3d's (index << 32) ones will do. */
#define FAKE_GPU_HANDLE_BIT (1ull << 62)

static SIZE_T APIENTRY calc_descriptor_heap_size(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 *args) {
    (void)hDevice;
    (void)args;
    return sizeof(D3D12_UMD_DescriptorHeap);
}

static HRESULT APIENTRY create_descriptor_heap(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 *args,
                                               D3D12DDI_HDESCRIPTORHEAP handle) {
    D3D12_UMD_Device *device = umd_device(hDevice);
    D3D12_UMD_DescriptorHeap *heap = handle.pDrvPrivate;
    memset(heap, 0, sizeof(*heap));

    /* DDI 0x1 is CPU_VISIBLE, API 0x1 is SHADER_VISIBLE. */
    bool visible = args->Flags & D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    D3D12_DESCRIPTOR_HEAP_DESC desc = {
        .Type = (D3D12_DESCRIPTOR_HEAP_TYPE)args->Type,
        .NumDescriptors = args->NumDescriptors,
        .Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
    };
    HRESULT hr = ID3D12Device_CreateDescriptorHeap(device->d3d12, &desc, &IID_ID3D12DescriptorHeap, (void **)&heap->heap);
    if (FAILED(hr)) {
        ERROR("%s: type %d, %u descriptors, flags 0x%x: 0x%08lx", __FUNCTION__, args->Type, args->NumDescriptors,
              args->Flags, hr);
        return hr;
    }
    heap->cpu.ptr = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap->heap).ptr;
    heap->gpu.ptr = visible ? ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap->heap).ptr
                            : FAKE_GPU_HANDLE_BIT | heap->cpu.ptr;
    if (d3d12_umd_log_enabled())
        INFO("descriptor heap %p: type %d, %u descriptors, flags 0x%x, cpu 0x%llx gpu 0x%llx", heap, args->Type,
             args->NumDescriptors, args->Flags, (unsigned long long)heap->cpu.ptr, (unsigned long long)heap->gpu.ptr);
    return S_OK;
}

static void APIENTRY destroy_descriptor_heap(D3D12DDI_HDEVICE hDevice, D3D12DDI_HDESCRIPTORHEAP handle) {
    (void)hDevice;
    D3D12_UMD_DescriptorHeap *heap = handle.pDrvPrivate;
    if (heap->heap)
        ID3D12DescriptorHeap_Release(heap->heap);
    memset(heap, 0, sizeof(*heap));
}

static D3D12DDI_CPU_DESCRIPTOR_HANDLE APIENTRY get_cpu_descriptor_handle_for_heap_start(D3D12DDI_HDEVICE hDevice,
                                                                                        D3D12DDI_HDESCRIPTORHEAP handle) {
    (void)hDevice;
    return ((D3D12_UMD_DescriptorHeap *)handle.pDrvPrivate)->cpu;
}

static D3D12DDI_GPU_DESCRIPTOR_HANDLE APIENTRY get_gpu_descriptor_handle_for_heap_start(D3D12DDI_HDEVICE hDevice,
                                                                                        D3D12DDI_HDESCRIPTORHEAP handle) {
    (void)hDevice;
    return ((D3D12_UMD_DescriptorHeap *)handle.pDrvPrivate)->gpu;
}

static inline D3D12_CPU_DESCRIPTOR_HANDLE dest_handle(D3D12DDI_CPU_DESCRIPTOR_HANDLE h) {
    return (D3D12_CPU_DESCRIPTOR_HANDLE) { h.ptr };
}

/* ---------------------------------------------------------------------------
 * Views. The DDI gives the resource dimension, not the view dimension: array,
 * multisample and cube-array views have to be told apart from the resource
 * and the range. On Adreno an array view of one layer and a plain view share
 * a descriptor, so erring towards arrays is harmless.
 */

static UINT resource_layers(const D3D12_UMD_Resource *r) {
    return r && r->desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D ? r->desc.DepthOrArraySize : 1;
}

static bool resource_multisampled(const D3D12_UMD_Resource *r) {
    return r && r->desc.SampleDesc.Count > 1;
}

static bool is_array(const D3D12_UMD_Resource *r, UINT first, UINT count) {
    return first || count > 1 || resource_layers(r) > 1;
}

static void APIENTRY create_shader_resource_view(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002 *args,
                                                 D3D12DDI_CPU_DESCRIPTOR_HANDLE dest) {
    D3D12_UMD_Device *device = umd_device(hDevice);
    D3D12_UMD_Resource *r = umd_resource(args->hDrvResource);
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {
        .Format = args->Format,
        /* Same encoding as D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING. */
        .Shader4ComponentMapping = args->Shader4ComponentMapping,
    };

    switch (args->ResourceDimension) {
    case D3D12DDI_RD_BUFFER:
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Buffer = (D3D12_BUFFER_SRV) {
            args->Buffer.FirstElement, args->Buffer.NumElements, args->Buffer.StructureByteStride,
            (D3D12_BUFFER_SRV_FLAGS)args->Buffer.Flags,
        };
        break;
    case D3D12DDI_RD_TEXTURE1D: {
        const D3D12DDIARG_TEX1D_SHADER_RESOURCE_VIEW *v = &args->Tex1D;
        if (is_array(r, v->FirstArraySlice, v->ArraySize)) {
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
            d.Texture1DArray = (D3D12_TEX1D_ARRAY_SRV) {
                v->MostDetailedMip, v->MipLevels, v->FirstArraySlice, v->ArraySize, v->ResourceMinLODClamp,
            };
        } else {
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
            d.Texture1D = (D3D12_TEX1D_SRV) { v->MostDetailedMip, v->MipLevels, v->ResourceMinLODClamp };
        }
        break;
    }
    case D3D12DDI_RD_TEXTURE2D: {
        const D3D12DDIARG_TEX2D_SHADER_RESOURCE_VIEW_0002 *v = &args->Tex2D;
        bool array = is_array(r, v->FirstArraySlice, v->ArraySize);
        if (resource_multisampled(r) && array) {
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
            d.Texture2DMSArray = (D3D12_TEX2DMS_ARRAY_SRV) { v->FirstArraySlice, v->ArraySize };
        } else if (resource_multisampled(r)) {
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        } else if (array) {
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray = (D3D12_TEX2D_ARRAY_SRV) {
                v->MostDetailedMip, v->MipLevels, v->FirstArraySlice, v->ArraySize, v->PlaneSlice, v->ResourceMinLODClamp,
            };
        } else {
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            d.Texture2D = (D3D12_TEX2D_SRV) { v->MostDetailedMip, v->MipLevels, v->PlaneSlice, v->ResourceMinLODClamp };
        }
        break;
    }
    case D3D12DDI_RD_TEXTURE3D:
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        d.Texture3D = (D3D12_TEX3D_SRV) { args->Tex3D.MostDetailedMip, args->Tex3D.MipLevels, args->Tex3D.ResourceMinLODClamp };
        break;
    case D3D12DDI_RD_TEXTURECUBE: {
        const D3D12DDIARG_TEXCUBE_SHADER_RESOURCE_VIEW *v = &args->TexCube;
        if (v->First2DArrayFace || v->NumCubes > 1 || resource_layers(r) > 6) {
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            d.TextureCubeArray = (D3D12_TEXCUBE_ARRAY_SRV) {
                v->MostDetailedMip, v->MipLevels, v->First2DArrayFace, v->NumCubes, v->ResourceMinLODClamp,
            };
        } else {
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            d.TextureCube = (D3D12_TEXCUBE_SRV) { v->MostDetailedMip, v->MipLevels, v->ResourceMinLODClamp };
        }
        break;
    }
    default:
        ERROR("%s: resource dimension %d", __FUNCTION__, args->ResourceDimension);
        d3d12_umd_set_error(device, E_INVALIDARG);
        return;
    }
    ID3D12Device_CreateShaderResourceView(device->d3d12, r ? r->res : NULL, &d, dest_handle(dest));
}

static void APIENTRY create_unordered_access_view(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002 *args,
                                                  D3D12DDI_CPU_DESCRIPTOR_HANDLE dest) {
    D3D12_UMD_Device *device = umd_device(hDevice);
    D3D12_UMD_Resource *r = umd_resource(args->hDrvResource);
    ID3D12Resource *counter = NULL;
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = { .Format = args->Format };

    switch (args->ResourceDimension) {
    case D3D12DDI_RD_BUFFER:
        d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        d.Buffer = (D3D12_BUFFER_UAV) {
            args->Buffer.FirstElement, args->Buffer.NumElements, args->Buffer.StructureByteStride,
            args->Buffer.CounterOffsetInBytes, (D3D12_BUFFER_UAV_FLAGS)args->Buffer.Flags,
        };
        counter = umd_res(args->Buffer.hDrvCounterResource);
        break;
    case D3D12DDI_RD_TEXTURE1D: {
        const D3D12DDIARG_TEX1D_UNORDERED_ACCESS_VIEW *v = &args->Tex1D;
        if (is_array(r, v->FirstArraySlice, v->ArraySize)) {
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
            d.Texture1DArray = (D3D12_TEX1D_ARRAY_UAV) { v->MipSlice, v->FirstArraySlice, v->ArraySize };
        } else {
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
            d.Texture1D = (D3D12_TEX1D_UAV) { v->MipSlice };
        }
        break;
    }
    case D3D12DDI_RD_TEXTURE2D: {
        const D3D12DDIARG_TEX2D_UNORDERED_ACCESS_VIEW_0002 *v = &args->Tex2D;
        if (is_array(r, v->FirstArraySlice, v->ArraySize)) {
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray = (D3D12_TEX2D_ARRAY_UAV) { v->MipSlice, v->FirstArraySlice, v->ArraySize, v->PlaneSlice };
        } else {
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            d.Texture2D = (D3D12_TEX2D_UAV) { v->MipSlice, v->PlaneSlice };
        }
        break;
    }
    case D3D12DDI_RD_TEXTURE3D:
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        d.Texture3D = (D3D12_TEX3D_UAV) { args->Tex3D.MipSlice, args->Tex3D.FirstW, args->Tex3D.WSize };
        break;
    default:
        ERROR("%s: resource dimension %d", __FUNCTION__, args->ResourceDimension);
        d3d12_umd_set_error(device, E_INVALIDARG);
        return;
    }
    ID3D12Device_CreateUnorderedAccessView(device->d3d12, r ? r->res : NULL, counter, &d, dest_handle(dest));
}

static void APIENTRY create_render_target_view(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 *args,
                                               D3D12DDI_CPU_DESCRIPTOR_HANDLE dest) {
    D3D12_UMD_Device *device = umd_device(hDevice);
    D3D12_UMD_Resource *r = umd_resource(args->hDrvResource);
    D3D12_RENDER_TARGET_VIEW_DESC d = { .Format = args->Format };
    if (d3d12_umd_log_enabled() && args->ResourceDimension == D3D12DDI_RD_TEXTURE3D) {
        const void *volatile *slot = (const void *volatile *)dest.ptr;
        INFO("rtv 3d pre: dest 0x%llx fmt %u mip %u firstW %u wsize %u (res %p depth %u) slot[%llx %llx]",
             (unsigned long long)dest.ptr, args->Format, args->Tex3D.MipSlice,
             args->Tex3D.FirstW, args->Tex3D.WSize, r ? r->res : NULL, r ? r->desc.DepthOrArraySize : 0,
             (unsigned long long)slot[0], (unsigned long long)slot[2]);
    }

    switch (args->ResourceDimension) {
    case D3D12DDI_RD_BUFFER:
        d.ViewDimension = D3D12_RTV_DIMENSION_BUFFER;
        d.Buffer = (D3D12_BUFFER_RTV) { args->Buffer.FirstElement, args->Buffer.NumElements };
        break;
    case D3D12DDI_RD_TEXTURE1D: {
        const D3D12DDIARG_TEX1D_RENDER_TARGET_VIEW *v = &args->Tex1D;
        if (is_array(r, v->FirstArraySlice, v->ArraySize)) {
            d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
            d.Texture1DArray = (D3D12_TEX1D_ARRAY_RTV) { v->MipSlice, v->FirstArraySlice, v->ArraySize };
        } else {
            d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;
            d.Texture1D = (D3D12_TEX1D_RTV) { v->MipSlice };
        }
        break;
    }
    case D3D12DDI_RD_TEXTURE2D: {
        const D3D12DDIARG_TEX2D_RENDER_TARGET_VIEW_0002 *v = &args->Tex2D;
        bool array = is_array(r, v->FirstArraySlice, v->ArraySize);
        if (resource_multisampled(r) && array) {
            d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
            d.Texture2DMSArray = (D3D12_TEX2DMS_ARRAY_RTV) { v->FirstArraySlice, v->ArraySize };
        } else if (resource_multisampled(r)) {
            d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        } else if (array) {
            d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray = (D3D12_TEX2D_ARRAY_RTV) { v->MipSlice, v->FirstArraySlice, v->ArraySize, v->PlaneSlice };
        } else {
            d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            d.Texture2D = (D3D12_TEX2D_RTV) { v->MipSlice, v->PlaneSlice };
        }
        break;
    }
    case D3D12DDI_RD_TEXTURE3D:
        d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        d.Texture3D = (D3D12_TEX3D_RTV) { args->Tex3D.MipSlice, args->Tex3D.FirstW, args->Tex3D.WSize };
        break;
    case D3D12DDI_RD_TEXTURECUBE:
        /* The API has no cube render targets: faces are array layers. */
        d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray = (D3D12_TEX2D_ARRAY_RTV) { args->TexCube.MipSlice, args->TexCube.FirstArraySlice,
                                                     args->TexCube.ArraySize, 0 };
        break;
    default:
        ERROR("%s: resource dimension %d", __FUNCTION__, args->ResourceDimension);
        d3d12_umd_set_error(device, E_INVALIDARG);
        return;
    }
    ID3D12Device_CreateRenderTargetView(device->d3d12, r ? r->res : NULL, &d, dest_handle(dest));
    if (d3d12_umd_log_enabled() && args->ResourceDimension == D3D12DDI_RD_TEXTURE3D) {
        const void *volatile *slot = (const void *volatile *)dest.ptr;
        if (!slot[0] || !slot[2])
            ERROR("rtv 3d post: vkd3d left the descriptor unwritten (view=%p res=%p fmt=%p)", slot[0], slot[1], slot[2]);
        else
            INFO("rtv 3d post: view=%p res=%p fmt=%p", slot[0], slot[1], slot[2]);
    }
}

static void APIENTRY create_depth_stencil_view(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_DEPTH_STENCIL_VIEW *args,
                                               D3D12DDI_CPU_DESCRIPTOR_HANDLE dest) {
    D3D12_UMD_Device *device = umd_device(hDevice);
    D3D12_UMD_Resource *r = umd_resource(args->hDrvResource);
    /* READ_ONLY_DEPTH / READ_ONLY_STENCIL share their values with the API. */
    D3D12_DEPTH_STENCIL_VIEW_DESC d = {
        .Format = args->Format,
        .Flags = (D3D12_DSV_FLAGS)(args->Flags & D3D12DDI_CREATE_DSV_FLAG_MASK),
    };

    switch (args->ResourceDimension) {
    case D3D12DDI_RD_TEXTURE1D: {
        const D3D12DDIARG_TEX1D_DEPTH_STENCIL_VIEW *v = &args->Tex1D;
        if (is_array(r, v->FirstArraySlice, v->ArraySize)) {
            d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
            d.Texture1DArray = (D3D12_TEX1D_ARRAY_DSV) { v->MipSlice, v->FirstArraySlice, v->ArraySize };
        } else {
            d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
            d.Texture1D = (D3D12_TEX1D_DSV) { v->MipSlice };
        }
        break;
    }
    case D3D12DDI_RD_TEXTURE2D:
    case D3D12DDI_RD_TEXTURECUBE: {
        /* Same shape for both; cube faces are array layers. */
        const D3D12DDIARG_TEX2D_DEPTH_STENCIL_VIEW *v = &args->Tex2D;
        bool array = args->ResourceDimension == D3D12DDI_RD_TEXTURECUBE || is_array(r, v->FirstArraySlice, v->ArraySize);
        if (resource_multisampled(r) && array) {
            d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
            d.Texture2DMSArray = (D3D12_TEX2DMS_ARRAY_DSV) { v->FirstArraySlice, v->ArraySize };
        } else if (resource_multisampled(r)) {
            d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
        } else if (array) {
            d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray = (D3D12_TEX2D_ARRAY_DSV) { v->MipSlice, v->FirstArraySlice, v->ArraySize };
        } else {
            d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            d.Texture2D = (D3D12_TEX2D_DSV) { v->MipSlice };
        }
        break;
    }
    default:
        ERROR("%s: resource dimension %d", __FUNCTION__, args->ResourceDimension);
        d3d12_umd_set_error(device, E_INVALIDARG);
        return;
    }
    if (d3d12_umd_log_enabled())
        INFO("create dsv: resource=%p desc=%p fmt=%u dim=%u flags=0x%x view_dim=%u dest=0x%llx "
             "resource_desc={dim=%u %llux%ux%u mips=%u fmt=%u samples=%u flags=0x%x}",
             r ? r->res : NULL, (void *)args->hDrvResource.pDrvPrivate, args->Format, args->ResourceDimension,
             args->Flags, d.ViewDimension, (unsigned long long)dest.ptr,
             r ? r->desc.Dimension : 0, r ? (unsigned long long)r->desc.Width : 0, r ? r->desc.Height : 0,
             r ? r->desc.DepthOrArraySize : 0, r ? r->desc.MipLevels : 0, r ? r->desc.Format : 0,
             r ? r->desc.SampleDesc.Count : 0, r ? r->desc.Flags : 0);
    ID3D12Device_CreateDepthStencilView(device->d3d12, r ? r->res : NULL, &d, dest_handle(dest));
}

static void APIENTRY create_constant_buffer_view(D3D12DDI_HDEVICE hDevice, const D3D12DDI_CONSTANT_BUFFER_VIEW_DESC *args,
                                                 D3D12DDI_CPU_DESCRIPTOR_HANDLE dest) {
    D3D12_UMD_Device *device = umd_device(hDevice);
    D3D12_CONSTANT_BUFFER_VIEW_DESC d;
    if (args)
        d = (D3D12_CONSTANT_BUFFER_VIEW_DESC) { args->BufferLocation, args->SizeInBytes };
    ID3D12Device_CreateConstantBufferView(device->d3d12, args ? &d : NULL, dest_handle(dest));
}

static void APIENTRY create_sampler(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_SAMPLER *args,
                                    D3D12DDI_CPU_DESCRIPTOR_HANDLE dest) {
    /* Field for field and value for value the API's sampler desc. */
    static_assert(sizeof(D3D12DDI_SAMPLER_DESC) == sizeof(D3D12_SAMPLER_DESC));
    static_assert(offsetof(D3D12DDI_SAMPLER_DESC, BorderColor) == offsetof(D3D12_SAMPLER_DESC, BorderColor));
    D3D12_UMD_Device *device = umd_device(hDevice);
    ID3D12Device_CreateSampler(device->d3d12, (const D3D12_SAMPLER_DESC *)args->pSamplerDesc, dest_handle(dest));
}

static void APIENTRY copy_descriptors(D3D12DDI_HDEVICE hDevice, UINT dst_count, const D3D12DDI_CPU_DESCRIPTOR_HANDLE *dst_starts,
                                      const UINT *dst_sizes, UINT src_count, const D3D12DDI_CPU_DESCRIPTOR_HANDLE *src_starts,
                                      const UINT *src_sizes, D3D12DDI_DESCRIPTOR_HEAP_TYPE type) {
    D3D12_UMD_Device *device = umd_device(hDevice);
    ID3D12Device_CopyDescriptors(device->d3d12, dst_count, (const D3D12_CPU_DESCRIPTOR_HANDLE *)dst_starts, dst_sizes,
                                 src_count, (const D3D12_CPU_DESCRIPTOR_HANDLE *)src_starts, src_sizes,
                                 (D3D12_DESCRIPTOR_HEAP_TYPE)type);
}

static void APIENTRY copy_descriptors_simple(D3D12DDI_HDEVICE hDevice, UINT count, D3D12DDI_CPU_DESCRIPTOR_HANDLE dst,
                                             D3D12DDI_CPU_DESCRIPTOR_HANDLE src, D3D12DDI_DESCRIPTOR_HEAP_TYPE type) {
    D3D12_UMD_Device *device = umd_device(hDevice);
    ID3D12Device_CopyDescriptorsSimple(device->d3d12, count, dest_handle(dst), dest_handle(src),
                                       (D3D12_DESCRIPTOR_HEAP_TYPE)type);
}

/* ---------------------------------------------------------------------------
 * Query heaps and command signatures: the same descriptions as the API's.
 */

static SIZE_T APIENTRY calc_query_heap_size(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_QUERY_HEAP_0001 *args) {
    (void)hDevice;
    (void)args;
    return sizeof(D3D12_UMD_QueryHeap);
}

static HRESULT APIENTRY create_query_heap(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_QUERY_HEAP_0001 *args,
                                          D3D12DDI_HQUERYHEAP handle) {
    D3D12_UMD_Device *device = umd_device(hDevice);
    D3D12_UMD_QueryHeap *heap = handle.pDrvPrivate;
    D3D12_QUERY_HEAP_DESC desc = { (D3D12_QUERY_HEAP_TYPE)args->Type, args->Count, 0 };
    heap->heap = NULL;
    HRESULT hr = ID3D12Device_CreateQueryHeap(device->d3d12, &desc, &IID_ID3D12QueryHeap, (void **)&heap->heap);
    if (FAILED(hr))
        ERROR("%s: type %d count %u: 0x%08lx", __FUNCTION__, args->Type, args->Count, hr);
    return hr;
}

static void APIENTRY destroy_query_heap(D3D12DDI_HDEVICE hDevice, D3D12DDI_HQUERYHEAP handle) {
    (void)hDevice;
    D3D12_UMD_QueryHeap *heap = handle.pDrvPrivate;
    if (heap->heap)
        ID3D12QueryHeap_Release(heap->heap);
    heap->heap = NULL;
}

static SIZE_T APIENTRY calc_command_signature_size(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 *args) {
    (void)hDevice;
    (void)args;
    return sizeof(D3D12_UMD_CommandSignature);
}

static HRESULT APIENTRY create_command_signature(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 *args,
                                                 D3D12DDI_HCOMMANDSIGNATURE handle) {
    static_assert(sizeof(D3D12DDI_INDIRECT_ARGUMENT_DESC) == sizeof(D3D12_INDIRECT_ARGUMENT_DESC));
    static_assert(offsetof(D3D12DDI_INDIRECT_ARGUMENT_DESC, Type) == offsetof(D3D12_INDIRECT_ARGUMENT_DESC, Type));
    static_assert(offsetof(D3D12DDI_INDIRECT_ARGUMENT_DESC, Constant) == offsetof(D3D12_INDIRECT_ARGUMENT_DESC, Constant));
    D3D12_UMD_Device *device = umd_device(hDevice);
    D3D12_UMD_CommandSignature *sig = handle.pDrvPrivate;
    D3D12_COMMAND_SIGNATURE_DESC desc = {
        .ByteStride = args->ByteStride,
        .NumArgumentDescs = args->NumArgumentDescs,
        .pArgumentDescs = (const D3D12_INDIRECT_ARGUMENT_DESC *)args->pArgumentDescs,
        .NodeMask = args->NodeMask,
    };
    sig->sig = NULL;
    HRESULT hr = ID3D12Device_CreateCommandSignature(device->d3d12, &desc, umd_root_signature(args->hRootSignature),
                                                     &IID_ID3D12CommandSignature, (void **)&sig->sig);
    if (d3d12_umd_log_enabled())
        INFO("command signature stride=%u args=%u root=%p node=0x%x -> 0x%08lx", args->ByteStride,
             args->NumArgumentDescs, umd_root_signature(args->hRootSignature), args->NodeMask, hr);
    if (FAILED(hr))
        ERROR("%s: stride %u, %u arguments: 0x%08lx", __FUNCTION__, args->ByteStride, args->NumArgumentDescs, hr);
    return hr;
}

static void APIENTRY destroy_command_signature(D3D12DDI_HDEVICE hDevice, D3D12DDI_HCOMMANDSIGNATURE handle) {
    (void)hDevice;
    D3D12_UMD_CommandSignature *sig = handle.pDrvPrivate;
    if (sig->sig)
        ID3D12CommandSignature_Release(sig->sig);
    sig->sig = NULL;
}

void d3d12_umd_install_descriptor_core(D3D12DDI_DEVICE_FUNCS_CORE_0040 *t) {
    t->pfnCalcPrivateDescriptorHeapSize = calc_descriptor_heap_size;
    t->pfnCreateDescriptorHeap = create_descriptor_heap;
    t->pfnDestroyDescriptorHeap = destroy_descriptor_heap;
    t->pfnGetCPUDescriptorHandleForHeapStart = get_cpu_descriptor_handle_for_heap_start;
    t->pfnGetGPUDescriptorHandleForHeapStart = get_gpu_descriptor_handle_for_heap_start;
    t->pfnCreateShaderResourceView = create_shader_resource_view;
    t->pfnCreateConstantBufferView = create_constant_buffer_view;
    t->pfnCreateSampler = create_sampler;
    t->pfnCreateUnorderedAccessView = create_unordered_access_view;
    t->pfnCreateRenderTargetView = create_render_target_view;
    t->pfnCreateDepthStencilView = create_depth_stencil_view;
    t->pfnCopyDescriptors = copy_descriptors;
    t->pfnCopyDescriptorsSimple = copy_descriptors_simple;
    t->pfnCalcPrivateQueryHeapSize = calc_query_heap_size;
    t->pfnCreateQueryHeap = create_query_heap;
    t->pfnDestroyQueryHeap = destroy_query_heap;
    t->pfnCalcPrivateCommandSignatureSize = calc_command_signature_size;
    t->pfnCreateCommandSignature = create_command_signature;
    t->pfnDestroyCommandSignature = destroy_command_signature;
}
