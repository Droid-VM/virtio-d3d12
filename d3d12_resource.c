#include "d3d12_umd.h"

/* The Windows SDK used to build this UMD predates D3D12 tight alignment. */
#ifndef D3D12_RESOURCE_FLAG_USE_TIGHT_ALIGNMENT
#define D3D12_RESOURCE_FLAG_USE_TIGHT_ALIGNMENT ((D3D12_RESOURCE_FLAGS)0x400)
#endif

/* The WDK used for this UMD predates GPU upload heaps. */
#ifndef D3D12_HEAP_TYPE_GPU_UPLOAD
#define D3D12_HEAP_TYPE_GPU_UPLOAD ((D3D12_HEAP_TYPE)5)
#endif

/* ---------------------------------------------------------------------------
 * Heaps and resources (dx12plan.md §2.3 / §2.5).
 *
 * The DDI fuses heap and resource creation into one call; which halves are
 * present says what the application asked for:
 *
 *   heap + resource  a committed resource, or a heap with the buffer the
 *                    runtime creates over it
 *   heap only        a heap
 *   resource only    a placed resource: ReuseBufferGPUVA names a buffer of
 *                    the target heap and the offset within it
 *
 * Buffers and heaps become a vkd3d heap with the buffer placed at offset 0, so
 * placement and MapHeap always have an ID3D12Heap and a mappable span; a
 * committed texture stays a vkd3d committed resource.
 */

/* Resource flags: the DDI allows shader access where the API denies it, and
 * the bits sit elsewhere. */
static D3D12_RESOURCE_FLAGS resource_flags(D3D12DDI_RESOURCE_FLAGS_0003 ddi, bool buffer) {
    static const struct { UINT ddi, api; } map[] = {
        { D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET },
        { D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL },
        { D3D12DDI_RESOURCE_FLAG_0022_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS },
        { D3D12DDI_RESOURCE_FLAG_0003_CROSS_ADAPTER, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER },
        { D3D12DDI_RESOURCE_FLAG_0003_SIMULTANEOUS_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS },
        { D3D12DDI_RESOURCE_FLAG_0020_VIDEO_DECODE_REFERENCE_ONLY, D3D12_RESOURCE_FLAG_VIDEO_DECODE_REFERENCE_ONLY },
        { D3D12DDI_RESOURCE_FLAG_0111_USE_TIGHT_ALIGNMENT, D3D12_RESOURCE_FLAG_USE_TIGHT_ALIGNMENT },
        { D3D12DDI_RESOURCE_FLAG_0088_RAYTRACING_ACCELERATION_STRUCTURE,
          D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE },
    };
    D3D12_RESOURCE_FLAGS api = D3D12_RESOURCE_FLAG_NONE;
    for (size_t i = 0; i < ARRAY_SIZE(map); i++)
        if (ddi & map[i].ddi)
            api |= map[i].api;
    /* The API can only deny shader access to depth-stencil textures. */
    if (!buffer && (ddi & D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL) &&
        !(ddi & D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE))
        api |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    return api;
}

void d3d12_umd_resource_desc(const D3D12DDIARG_CREATERESOURCE_0003 *args, D3D12_RESOURCE_DESC *desc) {
    /* Dimension and layout enums share their values with the API. */
    bool buffer = args->ResourceType == D3D12DDI_RT_BUFFER;
    *desc = (D3D12_RESOURCE_DESC) {
        .Dimension = (D3D12_RESOURCE_DIMENSION)args->ResourceType,
        .Width = args->Width,
        .Height = args->Height,
        .DepthOrArraySize = args->DepthOrArraySize,
        .MipLevels = args->MipLevels,
        .Format = args->Format,
        .SampleDesc = args->SampleDesc,
        .Layout = buffer ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR : (D3D12_TEXTURE_LAYOUT)args->Layout,
        .Flags = resource_flags(args->Flags, buffer),
    };
}

/* The runtime turns heap types into custom properties for this adapter's
 * architecture; memory pool and page property sit one below the API's. The
 * adapter is UMA, where vkd3d refuses L1. */
static D3D12_HEAP_PROPERTIES heap_properties(const D3D12DDIARG_CREATEHEAP_0001 *args) {
    /* The runtime represents GPU_UPLOAD heaps as coherent system-wide DDI
     * heaps. Preserve the non-custom heap type so vkd3d can select its
     * staging-copy path for CPU-visible textures, including depth images. */
    D3D12_HEAP_TYPE type = (args->Flags & D3D12DDI_HEAP_FLAG_COHERENT_SYSTEMWIDE)
            ? D3D12_HEAP_TYPE_GPU_UPLOAD : D3D12_HEAP_TYPE_CUSTOM;

    return (D3D12_HEAP_PROPERTIES) {
        .Type = type,
        .CPUPageProperty = (D3D12_CPU_PAGE_PROPERTY)(args->CPUPageProperty + 1),
        .MemoryPoolPreference = (D3D12_MEMORY_POOL)(args->MemoryPool + 1),
    };
}

static D3D12_HEAP_DESC heap_desc(const D3D12DDIARG_CREATEHEAP_0001 *args) {
    D3D12_HEAP_FLAGS flags = D3D12_HEAP_FLAG_NONE;

    /* DDI flags name the resource classes that are allowed. The API flags
     * name the classes that are denied, so only translate the single-class
     * cases; the common all-classes case is D3D12_HEAP_FLAG_NONE. */
    if ((args->Flags & D3D12DDI_HEAP_FLAG_BUFFERS) &&
            !(args->Flags & (D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES | D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES)))
        flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    else if ((args->Flags & D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES) &&
            !(args->Flags & (D3D12DDI_HEAP_FLAG_BUFFERS | D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES)))
        flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
    else if ((args->Flags & D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES) &&
            !(args->Flags & (D3D12DDI_HEAP_FLAG_BUFFERS | D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES)))
        flags = D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;

    return (D3D12_HEAP_DESC) {
        .SizeInBytes = args->ByteSize,
        .Properties = heap_properties(args),
        .Alignment = args->Alignment > D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT
                         ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                         : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
        .Flags = flags,
    };
}

static const D3D12_CLEAR_VALUE *clear_value(const D3D12_RESOURCE_DESC *desc, const D3D12DDI_CLEAR_VALUES *clear) {
    static_assert(sizeof(D3D12DDI_CLEAR_VALUES) == sizeof(D3D12_CLEAR_VALUE));
    /* The API rejects clear values on anything but render targets and depth. */
    if (!clear || !(desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)))
        return NULL;
    return (const D3D12_CLEAR_VALUE *)clear;
}

static D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY calc_heap_and_resource_sizes(
        D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATEHEAP_0001 *heap, const D3D12DDIARG_CREATERESOURCE_0003 *resource,
        D3D12DDI_HPROTECTEDRESOURCESESSION_0030 session) {
    (void)hDevice;
    (void)session;
    return (D3D12DDI_HEAP_AND_RESOURCE_SIZES) {
        .Heap = heap ? sizeof(D3D12_UMD_Heap) : 0,
        .Resource = resource ? sizeof(D3D12_UMD_Resource) : 0,
    };
}

static void log_create(const char *what, const D3D12DDIARG_CREATEHEAP_0001 *heap, const D3D12DDIARG_CREATERESOURCE_0003 *res,
                       HRESULT hr) {
    if (!d3d12_umd_log_enabled() && SUCCEEDED(hr))
        return;
    char heap_text[160] = "", res_text[256] = "";
    if (heap)
        snprintf(heap_text, sizeof(heap_text), " heap{size 0x%llx align 0x%llx pool %d page %d flags 0x%x}",
                 (unsigned long long)heap->ByteSize, (unsigned long long)heap->Alignment, heap->MemoryPool,
                 heap->CPUPageProperty, heap->Flags);
    if (res)
        snprintf(res_text, sizeof(res_text),
                 " res{type %d %llux%ux%u mips %u fmt %u samples %u layout %d flags 0x%x state 0x%x parent %p+0x%llx}",
                 res->ResourceType, (unsigned long long)res->Width, res->Height, res->DepthOrArraySize, res->MipLevels,
                 res->Format, res->SampleDesc.Count, res->Layout, res->Flags, res->InitialResourceState,
                 res->ReuseBufferGPUVA.BaseAddress.UMD.hResource.pDrvPrivate,
                 (unsigned long long)res->ReuseBufferGPUVA.BaseAddress.UMD.Offset);
    if (FAILED(hr))
        ERROR("%s%s%s: 0x%08lx", what, heap_text, res_text, hr);
    else
        INFO("%s%s%s", what, heap_text, res_text);
}

static HRESULT create_heap_span(D3D12_UMD_Device *device, D3D12_UMD_Heap *heap, const D3D12DDIARG_CREATEHEAP_0001 *args,
                                const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state) {
    heap->desc = heap_desc(args);
    /* Committed resources use UINT64_MAX as the runtime's heap-size
     * sentinel. It is not an allocation size; size the backing heap from
     * the resource before handing it to vkd3d. */
    if (heap->desc.SizeInBytes == UINT64_MAX && desc) {
        D3D12_RESOURCE_ALLOCATION_INFO info = ID3D12Device_GetResourceAllocationInfo(device->d3d12, 0, 1, desc);
        if (!info.SizeInBytes || info.SizeInBytes == UINT64_MAX)
            return E_INVALIDARG;
        heap->desc.SizeInBytes = info.SizeInBytes;
        heap->desc.Alignment = info.Alignment;
    }
    HRESULT hr = ID3D12Device_CreateHeap(device->d3d12, &heap->desc, &IID_ID3D12Heap, (void **)&heap->heap);
    if (FAILED(hr) || !desc)
        return hr;
    return ID3D12Device_CreatePlacedResource(device->d3d12, heap->heap, 0, desc, state, NULL, &IID_ID3D12Resource,
                                             (void **)&heap->span);
}

static HRESULT APIENTRY create_heap_and_resource(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATEHEAP_0001 *heap_args,
                                                 D3D12DDI_HHEAP hHeap, D3D12DDI_HRTRESOURCE hRTResource,
                                                 const D3D12DDIARG_CREATERESOURCE_0003 *res_args,
                                                 const D3D12DDI_CLEAR_VALUES *clear,
                                                 D3D12DDI_HPROTECTEDRESOURCESESSION_0030 session,
                                                 D3D12DDI_HRESOURCE hResource) {
    (void)session;
    D3D12_UMD_Device *device = umd_device(hDevice);
    D3D12_UMD_Heap *heap = heap_args ? hHeap.pDrvPrivate : NULL;
    D3D12_UMD_Resource *resource = res_args ? hResource.pDrvPrivate : NULL;
    if (heap)
        memset(heap, 0, sizeof(*heap));
    if (resource)
        memset(resource, 0, sizeof(*resource));

    D3D12_RESOURCE_DESC desc;
    if (res_args)
        d3d12_umd_resource_desc(res_args, &desc);
    D3D12_RESOURCE_STATES state = res_args ? (D3D12_RESOURCE_STATES)res_args->InitialResourceState : 0;

    HRESULT hr = E_INVALIDARG;
    const char *what = "?";
    if (heap_args && (heap_args->Flags & D3D12DDI_HEAP_FLAG_PRIMARY)) {
        what = "primary texture";
        if (heap && resource) {
            heap->desc = heap_desc(heap_args);
            resource->desc = desc;
            hr = d3d12_umd_create_primary(device, resource, hRTResource);
        }
    } else if (heap && resource && desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        what = "committed texture";
        D3D12_HEAP_PROPERTIES props = heap_properties(heap_args);
        hr = ID3D12Device_CreateCommittedResource(device->d3d12, &props, D3D12_HEAP_FLAG_NONE, &desc, state,
                                                  clear_value(&desc, clear), &IID_ID3D12Resource, (void **)&resource->res);
        if (SUCCEEDED(hr)) {
            heap->committed = resource->res;
            ID3D12Resource_AddRef(heap->committed);
            resource->heap = heap;
        }
    } else if (heap) {
        what = resource ? "heap + buffer" : "heap";
        hr = create_heap_span(device, heap, heap_args, resource ? &desc : NULL, state);
        if (SUCCEEDED(hr) && resource) {
            resource->res = heap->span;
            ID3D12Resource_AddRef(resource->res);
            resource->heap = heap;
        }
    } else if (resource) {
        what = "placed";
        const D3D12DDIARG_HRESOURCE_PLACEMENT *at = &res_args->ReuseBufferGPUVA.BaseAddress.UMD;
        D3D12_UMD_Resource *parent = umd_resource(at->hResource);
        if (parent && parent->heap && parent->heap->heap) {
            resource->heap = parent->heap;
            resource->heap_offset = parent->heap_offset + at->Offset;
            /* The DDI description omits the API Alignment member. Recover
             * small alignment for offsets below the default, but only when
             * vkd3d confirms that this texture supports it. */
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
                    !(desc.Flags & D3D12_RESOURCE_FLAG_USE_TIGHT_ALIGNMENT)) {
                UINT64 default_alignment = desc.SampleDesc.Count > 1
                        ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                        : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
                if (resource->heap_offset & (default_alignment - 1)) {
                    D3D12_RESOURCE_DESC small = desc;
                    small.Alignment = desc.SampleDesc.Count > 1
                            ? D3D12_SMALL_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                            : D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
                    D3D12_RESOURCE_ALLOCATION_INFO info =
                            ID3D12Device_GetResourceAllocationInfo(device->d3d12, 0, 1, &small);
                    if (info.SizeInBytes != UINT64_MAX && info.Alignment == small.Alignment &&
                            !(resource->heap_offset & (info.Alignment - 1)))
                        desc.Alignment = small.Alignment;
                }
            }
            hr = ID3D12Device_CreatePlacedResource(device->d3d12, resource->heap->heap, resource->heap_offset, &desc,
                                                   state, clear_value(&desc, clear), &IID_ID3D12Resource,
                                                   (void **)&resource->res);
        }
    }

    if (resource)
        resource->desc = desc;
    log_create(what, heap_args, res_args, hr);
    if (FAILED(hr)) {
        if (resource && resource->res)
            ID3D12Resource_Release(resource->res);
        if (resource)
            d3d12_umd_destroy_primary(device, resource);
        if (heap) {
            if (heap->span)
                ID3D12Resource_Release(heap->span);
            if (heap->committed)
                ID3D12Resource_Release(heap->committed);
            if (heap->heap)
                ID3D12Heap_Release(heap->heap);
            memset(heap, 0, sizeof(*heap));
        }
        if (resource)
            memset(resource, 0, sizeof(*resource));
    }
    return hr;
}

static void APIENTRY destroy_heap_and_resource(D3D12DDI_HDEVICE hDevice, D3D12DDI_HHEAP hHeap, D3D12DDI_HRESOURCE hResource) {
    D3D12_UMD_Device *device = umd_device(hDevice);
    D3D12_UMD_Resource *resource = hResource.pDrvPrivate;
    D3D12_UMD_Heap *heap = hHeap.pDrvPrivate;
    if (resource) {
        if (resource->res)
            ID3D12Resource_Release(resource->res);
        d3d12_umd_destroy_primary(device, resource);
        memset(resource, 0, sizeof(*resource));
    }
    if (heap) {
        if (heap->span)
            ID3D12Resource_Release(heap->span);
        if (heap->committed)
            ID3D12Resource_Release(heap->committed);
        if (heap->heap)
            ID3D12Heap_Release(heap->heap);
        memset(heap, 0, sizeof(*heap));
    }
}

/* win3d UMD bridge into the statically linked vkd3d (VKD3D_BUILD_UMD exports
 * in vkd3d-proton/libs/vkd3d/resource.c): the runtime's WriteToSubresource /
 * ReadFromSubresource and CPU-visible heap mapping are built on exactly the
 * host pointer and subresource layout that the inner implementation uses. */
extern HRESULT vkd3d_acquire_resource_cpu_pointer(ID3D12Resource *resource, void **data);
extern HRESULT vkd3d_get_resource_subresource_layout(ID3D12Resource *resource, UINT subresource,
        UINT64 *offset, UINT64 *row_pitch, UINT64 *depth_pitch);

/* The runtime adds the offset of what it maps; the heap maps from its start. */
static HRESULT APIENTRY map_heap(D3D12DDI_HDEVICE hDevice, D3D12DDI_HHEAP hHeap, void **data) {
    D3D12_UMD_Device *device = umd_device(hDevice);
    D3D12_UMD_Heap *heap = hHeap.pDrvPrivate;
    *data = NULL;
    if (!heap->span && !heap->committed && heap->heap) {
        /* A heap created without a buffer: give it one on first map. */
        D3D12_RESOURCE_DESC desc = {
            .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Width = heap->desc.SizeInBytes,
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .SampleDesc.Count = 1,
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        };
        HRESULT hr = ID3D12Device_CreatePlacedResource(device->d3d12, heap->heap, 0, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                       NULL, &IID_ID3D12Resource, (void **)&heap->span);
        if (FAILED(hr)) {
            ERROR("%s: heap %p: no span buffer: 0x%08lx", __FUNCTION__, heap, hr);
            return hr;
        }
    }
    /* The runtime still calls MapHeap for a committed texture on a custom
     * heap, even though an UNKNOWN-layout texture cannot expose a linear CPU
     * address.  Returning E_INVALIDARG here makes dxgkrnl report the device
     * removed; leave the address undisclosed and let the API layer return its
     * expected Map result instead. */
    if (heap->committed && !heap->span) {
        /* A texture on a CPU-visible heap is host memory laid out exactly as
         * the inner vkd3d sees it; hand the runtime that pointer so its
         * Map/WriteToSubresource/ReadFromSubresource actually reach the GPU
         * resource. GPU-only heaps keep the address undisclosed. */
        HRESULT hr2 = vkd3d_acquire_resource_cpu_pointer(heap->committed, data);
        if (SUCCEEDED(hr2)) {
            /* A NULL inner Map is legal for textures and invalidates the GPU
             * range; UnmapHeap later flushes what the runtime wrote. */
            ID3D12Resource_Map(heap->committed, 0, NULL, NULL);
            if (d3d12_umd_log_enabled())
                INFO("map heap %p committed texture: cpu ptr %p", heap, *data);
            return S_OK;
        }
        *data = NULL;
        return S_OK;
    }
    ID3D12Resource *res = heap->span ? heap->span : heap->committed;
    HRESULT hr = res ? ID3D12Resource_Map(res, 0, NULL, data) : E_INVALIDARG;
    if (d3d12_umd_log_enabled())
        INFO("map heap %p resource %p -> %p hr 0x%08lx", heap, res, *data, hr);
    if (FAILED(hr))
        ERROR("%s: heap %p: 0x%08lx", __FUNCTION__, heap, hr);
    return hr;
}

static void APIENTRY unmap_heap(D3D12DDI_HDEVICE hDevice, D3D12DDI_HHEAP hHeap) {
    (void)hDevice;
    D3D12_UMD_Heap *heap = hHeap.pDrvPrivate;
    ID3D12Resource *res = heap->span ? heap->span : heap->committed;
    if (res)
        ID3D12Resource_Unmap(res, 0, NULL);
}

/* ---------------------------------------------------------------------------
 * Layout queries.
 */

static void allocation_info(D3D12_UMD_Device *device, const D3D12_RESOURCE_DESC *desc, UINT32 alignment,
                            D3D12DDI_TEXTURE_LAYOUT layout, D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 *out) {
    memset(out, 0, sizeof(*out));
    bool buffer = desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;

    /* D3D12 buffer resources have a fixed 64 KiB placement alignment. The
     * runtime may pass the containing heap's 4 MiB alignment here; forwarding
     * that value makes vkd3d reject the otherwise valid buffer query with
     * UINT64_MAX. */
    if (buffer) {
        UINT32 buffer_alignment = alignment > D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT
                ? D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT : alignment;
        out->ResourceDataSize = desc->Width;
        out->ResourceDataAlignment = buffer_alignment ? buffer_alignment : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        out->AdditionalDataHeaderAlignment = 1;
        out->AdditionalDataAlignment = 1;
        out->Layout = D3D12DDI_TL_ROW_MAJOR;
        return;
    }

    D3D12_RESOURCE_DESC d = *desc;
    d.Alignment = alignment;
    D3D12_RESOURCE_ALLOCATION_INFO info = ID3D12Device_GetResourceAllocationInfo(device->d3d12, 0, 1, &d);

    out->ResourceDataSize = info.SizeInBytes;
    out->ResourceDataAlignment = (UINT32)info.Alignment;
    /* Zero is invalid for these DDI alignments even when no extra data is used. */
    out->AdditionalDataHeaderAlignment = 1;
    out->AdditionalDataAlignment = 1;
    out->Layout = buffer ? D3D12DDI_TL_ROW_MAJOR : layout;
    if (d3d12_umd_log_enabled())
        INFO("allocation info: dim %d %llux%ux%u fmt %u flags 0x%x align 0x%x -> size 0x%llx align 0x%llx",
             desc->Dimension, (unsigned long long)desc->Width, desc->Height, desc->DepthOrArraySize, desc->Format,
             desc->Flags, alignment, (unsigned long long)info.SizeInBytes, (unsigned long long)info.Alignment);
}

static void APIENTRY check_resource_allocation_info(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATERESOURCE_0003 *args,
                                                    D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS flags, UINT32 alignment,
                                                    UINT node_mask, D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 *out) {
    (void)flags;
    (void)node_mask;
    D3D12_RESOURCE_DESC desc;
    d3d12_umd_resource_desc(args, &desc);
    allocation_info(umd_device(hDevice), &desc, alignment, args->Layout, out);
}

static void APIENTRY check_existing_resource_allocation_info(D3D12DDI_HDEVICE hDevice, D3D12DDI_HRESOURCE hResource,
                                                             D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 *out) {
    D3D12_UMD_Resource *resource = umd_resource(hResource);
    allocation_info(umd_device(hDevice), &resource->desc, 0, (D3D12DDI_TEXTURE_LAYOUT)resource->desc.Layout, out);
}

static void APIENTRY check_subresource_info(D3D12DDI_HDEVICE hDevice, D3D12DDI_HRESOURCE hResource, UINT subresource,
                                            D3D12DDI_SUBRESOURCE_INFO *out) {
    (void)hDevice;
    D3D12_UMD_Resource *resource = umd_resource(hResource);
    memset(out, 0, sizeof(*out));
    if (resource->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        out->RowStride = resource->desc.Width;
        out->DepthStride = resource->desc.Width;
    } else if (SUCCEEDED(vkd3d_get_resource_subresource_layout(umd_res(hResource), subresource,
                    &out->Offset, &out->RowStride, &out->DepthStride))) {
        /* The inner layout of a host-visible texture: this is what the
         * runtime must use to address the pointer MapHeap returned. */
    } else {
        WARN("%s: texture %p subresource %u has no linear layout", __FUNCTION__, resource, subresource);
    }
}

static D3D12DDI_GPU_VIRTUAL_ADDRESS APIENTRY check_resource_virtual_address(D3D12DDI_HDEVICE hDevice,
                                                                           D3D12DDI_HRESOURCE hResource) {
    (void)hDevice;
    ID3D12Resource *res = umd_res(hResource);
    return res ? ID3D12Resource_GetGPUVirtualAddress(res) : 0;
}

static D3DKMT_HANDLE APIENTRY check_resource_allocation_handle(D3D12DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource) {
    (void)hDevice;
    (void)hResource;
    /* No kernel allocation backs vkd3d memory; primaries get one in P3. */
    return 0;
}

static void APIENTRY get_mip_packing(D3D12DDI_HDEVICE hDevice, D3D12DDI_HRESOURCE hResource, UINT *packed_mips,
                                     UINT *tiles) {
    (void)hDevice;
    (void)hResource;
    /* Tiled resources are not offered. */
    *packed_mips = 0;
    *tiles = 0;
}

static void APIENTRY get_debug_allocation_info(D3D12DDI_HDEVICE hDevice, D3D12DDI_HANDLE_AND_TYPE object, UINT *num_va,
                                               D3D12DDI_DEBUG_VIRTUAL_ADDRESS_ALLOCATION_INFO_0012 *va, UINT *num_kmt,
                                               D3D12DDI_DEBUG_KMT_ALLOCATION_INFO_0014 *kmt) {
    (void)hDevice;
    (void)object;
    (void)va;
    (void)kmt;
    if (num_va)
        *num_va = 0;
    if (num_kmt)
        *num_kmt = 0;
}

/* ---------------------------------------------------------------------------
 * Residency: vkd3d memory is always resident.
 */

static HRESULT APIENTRY make_resident(D3D12DDI_HDEVICE hDevice, D3D12DDIARG_MAKERESIDENT_0001 *args) {
    (void)hDevice;
    args->WaitMask = 0;
    return S_OK;
}

static HRESULT APIENTRY evict(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_EVICT *args) {
    (void)hDevice;
    (void)args;
    return S_OK;
}

static HRESULT APIENTRY offer_resources(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_OFFERRESOURCES *args) {
    (void)hDevice;
    (void)args;
    return S_OK;
}

static HRESULT APIENTRY reclaim_resources(D3D12DDI_HDEVICE hDevice, D3D12DDIARG_RECLAIMRESOURCES_0001 *args) {
    (void)hDevice;
    /* Nothing was ever given away, so nothing was discarded. */
    for (UINT i = 0; args->pDiscarded && i < args->NumObjects; i++)
        args->pDiscarded[i] = FALSE;
    args->WaitMask = 0;
    return S_OK;
}

void d3d12_umd_install_resource_core(D3D12DDI_DEVICE_FUNCS_CORE_0040 *t) {
    t->pfnCalcPrivateHeapAndResourceSizes = calc_heap_and_resource_sizes;
    t->pfnCreateHeapAndResource = create_heap_and_resource;
    t->pfnDestroyHeapAndResource = destroy_heap_and_resource;
    t->pfnMapHeap = map_heap;
    t->pfnUnmapHeap = unmap_heap;
    t->pfnCheckResourceAllocationInfo = check_resource_allocation_info;
    t->pfnCheckExistingResourceAllocationInfo = check_existing_resource_allocation_info;
    t->pfnCheckSubresourceInfo = check_subresource_info;
    t->pfnCheckResourceVirtualAddress = check_resource_virtual_address;
    t->pfnCheckResourceAllocationHandle = check_resource_allocation_handle;
    t->pfnGetMipPacking = get_mip_packing;
    t->pfnGetDebugAllocationInfo = get_debug_allocation_info;
    t->pfnMakeResident = make_resident;
    t->pfnEvict = evict;
    t->pfnOfferResources = offer_resources;
    t->pfnReclaimResources = reclaim_resources;
}
