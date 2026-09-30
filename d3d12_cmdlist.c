#include "d3d12_umd.h"

/* ---------------------------------------------------------------------------
 * Command pools and recorders: the 0040 split of the command allocator. The
 * runtime resets a list with a recorder, whose target pool stands for the
 * API allocator.
 */

static SIZE_T APIENTRY calc_command_pool_size(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_COMMAND_POOL_0040 *args) {
    (void)hDevice;
    (void)args;
    return sizeof(D3D12_UMD_CommandPool);
}

static HRESULT APIENTRY create_command_pool(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_COMMAND_POOL_0040 *args,
                                            D3D12DDI_HCOMMANDPOOL_0040 handle) {
    (void)args;
    D3D12_UMD_CommandPool *pool = handle.pDrvPrivate;
    memset(pool, 0, sizeof(*pool));
    pool->device = umd_device(hDevice);
    return S_OK;
}

static void APIENTRY destroy_command_pool(D3D12DDI_HDEVICE hDevice, D3D12DDI_HCOMMANDPOOL_0040 handle) {
    (void)hDevice;
    D3D12_UMD_CommandPool *pool = handle.pDrvPrivate;
    for (UINT i = 0; i < ARRAY_SIZE(pool->allocators); i++)
        if (pool->allocators[i])
            ID3D12CommandAllocator_Release(pool->allocators[i]);
    memset(pool, 0, sizeof(*pool));
}

static void APIENTRY reset_command_pool(D3D12DDI_HDEVICE hDevice, D3D12DDI_HCOMMANDPOOL_0040 handle) {
    (void)hDevice;
    D3D12_UMD_CommandPool *pool = handle.pDrvPrivate;
    for (UINT i = 0; i < ARRAY_SIZE(pool->allocators); i++) {
        if (!pool->allocators[i])
            continue;
        HRESULT hr = ID3D12CommandAllocator_Reset(pool->allocators[i]);
        if (FAILED(hr))
            WARN("%s: pool %p allocator %u reset failed: 0x%08lx", __FUNCTION__, pool, i, hr);
    }
}

static ID3D12CommandAllocator *pool_allocator(D3D12_UMD_CommandPool *pool, D3D12_COMMAND_LIST_TYPE type) {
    if ((UINT)type >= ARRAY_SIZE(pool->allocators))
        return NULL;
    if (!pool->allocators[type]) {
        HRESULT hr = ID3D12Device_CreateCommandAllocator(pool->device->d3d12, type, &IID_ID3D12CommandAllocator,
                                                         (void **)&pool->allocators[type]);
        if (FAILED(hr))
            ERROR("%s: pool %p type %d: 0x%08lx", __FUNCTION__, pool, type, hr);
    }
    return pool->allocators[type];
}

static SIZE_T APIENTRY calc_command_recorder_size(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_COMMAND_RECORDER_0040 *args) {
    (void)hDevice;
    (void)args;
    return sizeof(D3D12_UMD_CommandRecorder);
}

static HRESULT APIENTRY create_command_recorder(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_COMMAND_RECORDER_0040 *args,
                                                D3D12DDI_HCOMMANDRECORDER_0040 handle) {
    (void)hDevice;
    D3D12_UMD_CommandRecorder *recorder = handle.pDrvPrivate;
    recorder->queue_flags = args->QueueFlags;
    recorder->pool = NULL;
    return S_OK;
}

static void APIENTRY destroy_command_recorder(D3D12DDI_HDEVICE hDevice, D3D12DDI_HCOMMANDRECORDER_0040 handle) {
    (void)hDevice;
    (void)handle;
}

static void APIENTRY command_recorder_set_command_pool_as_target(D3D12DDI_HDEVICE hDevice,
                                                                 D3D12DDI_HCOMMANDRECORDER_0040 recorder,
                                                                 D3D12DDI_HCOMMANDPOOL_0040 pool) {
    (void)hDevice;
    ((D3D12_UMD_CommandRecorder *)recorder.pDrvPrivate)->pool = pool.pDrvPrivate;
}

/* ---------------------------------------------------------------------------
 * Command lists: one vkd3d list each, created closed; ResetCommandList opens
 * it on the recorder's pool.
 */

static D3D12_COMMAND_LIST_TYPE list_type(D3D12DDI_COMMAND_LIST_TYPE type, D3D12DDI_COMMAND_QUEUE_FLAGS queue_flags) {
    if (type == D3D12DDI_COMMAND_LIST_TYPE_BUNDLE)
        return D3D12_COMMAND_LIST_TYPE_BUNDLE;
    if (queue_flags & D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE)
        return D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (queue_flags & D3D12DDI_COMMAND_QUEUE_FLAG_COPY)
        return D3D12_COMMAND_LIST_TYPE_COPY;
    return D3D12_COMMAND_LIST_TYPE_DIRECT;
}

static SIZE_T APIENTRY calc_command_list_size(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_COMMAND_LIST_0040 *args) {
    (void)hDevice;
    (void)args;
    return sizeof(D3D12_UMD_CommandList);
}

static HRESULT APIENTRY create_command_list(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_COMMAND_LIST_0040 *args,
                                            D3D12DDI_HCOMMANDLIST handle, D3D12DDI_HRTCOMMANDLIST hRT) {
    D3D12_UMD_Device *device = umd_device(hDevice);
    D3D12_UMD_CommandList *list = handle.pDrvPrivate;
    memset(list, 0, sizeof(*list));
    list->device = device;
    list->hRT = hRT;
    list->type = list_type(args->Type, args->QueueFlags);

    HRESULT hr = ID3D12Device4_CreateCommandList1(device->d3d12_4, args->NodeMask, list->type, D3D12_COMMAND_LIST_FLAG_NONE,
                                                  &IID_ID3D12GraphicsCommandList2, (void **)&list->cl);
    if (FAILED(hr)) {
        ERROR("%s: type %d: 0x%08lx", __FUNCTION__, list->type, hr);
        return hr;
    }
    /* Without this the runtime routes the list to stubbed DDIs. */
    device->um->pfnSetCommandListDDITableCb(hRT, device->adapter->cl_tables[0]);
    return S_OK;
}

static void APIENTRY destroy_command_list(D3D12DDI_HDEVICE hDevice, D3D12DDI_HCOMMANDLIST handle) {
    (void)hDevice;
    D3D12_UMD_CommandList *list = umd_cl(handle);
    if (list->cl)
        ID3D12GraphicsCommandList2_Release(list->cl);
    memset(list, 0, sizeof(*list));
}

static void list_error(D3D12_UMD_CommandList *list, HRESULT hr) {
    ERROR("command list %p: 0x%08lx", list, hr);
    list->device->um->pfnSetCommandListErrorCb(list->hRT, hr);
}

static void APIENTRY cl_close(D3D12DDI_HCOMMANDLIST hCL) {
    D3D12_UMD_CommandList *list = umd_cl(hCL);
    HRESULT hr = ID3D12GraphicsCommandList2_Close(list->cl);
    if (d3d12_umd_log_enabled())
        INFO("close command list %p: hr=0x%08lx", list, hr);
    if (FAILED(hr))
        list_error(list, hr);
}

static void APIENTRY cl_reset(D3D12DDI_HCOMMANDLIST hCL, const D3D12DDIARG_RESETCOMMANDLIST_0040 *args) {
    umd_cl(hCL)->synchronous_present = false;
    D3D12_UMD_CommandList *list = umd_cl(hCL);
    D3D12_UMD_CommandRecorder *recorder = args->hDrvCommandRecorder.pDrvPrivate;
    ID3D12CommandAllocator *allocator = recorder && recorder->pool ? pool_allocator(recorder->pool, list->type) : NULL;
    if (!allocator) {
        list_error(list, E_INVALIDARG);
        return;
    }
    HRESULT hr = ID3D12GraphicsCommandList2_Reset(list->cl, allocator, NULL);
    if (FAILED(hr))
        list_error(list, hr);
}

/* ---------------------------------------------------------------------------
 * Recording. The DDI handles and argument structs mirror the API's, so most
 * slots are one call; handles resolve to the wrapped vkd3d objects.
 */

#define CL(h) (umd_cl(h)->cl)

static inline D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(D3D12DDI_CPU_DESCRIPTOR_HANDLE h) {
    return (D3D12_CPU_DESCRIPTOR_HANDLE) { h.ptr };
}

static inline D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle(D3D12DDI_GPU_DESCRIPTOR_HANDLE h) {
    return (D3D12_GPU_DESCRIPTOR_HANDLE) { h.ptr };
}

static_assert(sizeof(D3D12DDI_RECT) == sizeof(D3D12_RECT));
static_assert(sizeof(D3D12DDI_BOX) == sizeof(D3D12_BOX));
static_assert(sizeof(D3D12DDI_VIEWPORT) == sizeof(D3D12_VIEWPORT));
static_assert(sizeof(D3D12DDI_INDEX_BUFFER_VIEW) == sizeof(D3D12_INDEX_BUFFER_VIEW));
static_assert(sizeof(D3D12DDI_VERTEX_BUFFER_VIEW) == sizeof(D3D12_VERTEX_BUFFER_VIEW));
static_assert(sizeof(D3D12DDI_STREAM_OUTPUT_BUFFER_VIEW) == sizeof(D3D12_STREAM_OUTPUT_BUFFER_VIEW));
static_assert(sizeof(D3D12DDIARG_DISCARD_RESOURCE_0003) == sizeof(D3D12_DISCARD_REGION));
static_assert(sizeof(D3D12DDI_SAMPLE_POSITION) == sizeof(D3D12_SAMPLE_POSITION));
static_assert(sizeof(D3D12DDI_WRITEBUFFERIMMEDIATE_PARAMETER_0032) == sizeof(D3D12_WRITEBUFFERIMMEDIATE_PARAMETER));

static void APIENTRY cl_draw_instanced(D3D12DDI_HCOMMANDLIST hCL, UINT vertex_count, UINT instance_count,
                                       UINT start_vertex, UINT start_instance) {
    ID3D12GraphicsCommandList2_DrawInstanced(CL(hCL), vertex_count, instance_count, start_vertex, start_instance);
}

static void APIENTRY cl_draw_indexed_instanced(D3D12DDI_HCOMMANDLIST hCL, UINT index_count, UINT instance_count,
                                               UINT start_index, INT base_vertex, UINT start_instance) {
    ID3D12GraphicsCommandList2_DrawIndexedInstanced(CL(hCL), index_count, instance_count, start_index, base_vertex,
                                                    start_instance);
}

static void APIENTRY cl_dispatch(D3D12DDI_HCOMMANDLIST hCL, UINT x, UINT y, UINT z) {
    if (d3d12_umd_log_enabled())
        INFO("dispatch list=%p groups=%u,%u,%u", umd_cl(hCL), x, y, z);
    ID3D12GraphicsCommandList2_Dispatch(CL(hCL), x, y, z);
}

static void APIENTRY cl_clear_uav_uint(D3D12DDI_HCOMMANDLIST hCL, D3D12DDI_GPU_DESCRIPTOR_HANDLE gpu,
                                       D3D12DDI_CPU_DESCRIPTOR_HANDLE cpu, D3D12DDI_HRESOURCE resource,
                                       const UINT *values, UINT rect_count, const D3D12DDI_RECT *rects) {
    ID3D12GraphicsCommandList2_ClearUnorderedAccessViewUint(CL(hCL), gpu_handle(gpu), cpu_handle(cpu), umd_res(resource),
                                                            values, rect_count, (const D3D12_RECT *)rects);
}

static void APIENTRY cl_clear_uav_float(D3D12DDI_HCOMMANDLIST hCL, D3D12DDI_GPU_DESCRIPTOR_HANDLE gpu,
                                        D3D12DDI_CPU_DESCRIPTOR_HANDLE cpu, D3D12DDI_HRESOURCE resource,
                                        const FLOAT *values, UINT rect_count, const D3D12DDI_RECT *rects) {
    ID3D12GraphicsCommandList2_ClearUnorderedAccessViewFloat(CL(hCL), gpu_handle(gpu), cpu_handle(cpu), umd_res(resource),
                                                             values, rect_count, (const D3D12_RECT *)rects);
}

static void APIENTRY cl_clear_rtv(D3D12DDI_HCOMMANDLIST hCL, D3D12DDI_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT *color,
                                  UINT rect_count, const D3D12DDI_RECT *rects) {
    if (d3d12_umd_log_enabled())
        INFO("clear rtv 0x%llx", (unsigned long long)rtv.ptr);
    ID3D12GraphicsCommandList2_ClearRenderTargetView(CL(hCL), cpu_handle(rtv), color, rect_count, (const D3D12_RECT *)rects);
}

static void APIENTRY cl_clear_dsv(D3D12DDI_HCOMMANDLIST hCL, D3D12DDI_CPU_DESCRIPTOR_HANDLE dsv, UINT flags, FLOAT depth,
                                  UINT8 stencil, UINT rect_count, const D3D12DDI_RECT *rects) {
    /* D3D12DDI and D3D12 clear flags share values (DEPTH 1, STENCIL 2). */
    if (d3d12_umd_log_enabled())
        INFO("clear dsv list=%p dsv=0x%llx flags=0x%x depth=%f stencil=%u rects=%u",
             umd_cl(hCL), (unsigned long long)dsv.ptr, flags, depth, stencil, rect_count);
    ID3D12GraphicsCommandList2_ClearDepthStencilView(CL(hCL), cpu_handle(dsv), (D3D12_CLEAR_FLAGS)flags, depth, stencil,
                                                     rect_count, (const D3D12_RECT *)rects);
}

static void APIENTRY cl_discard_resource(D3D12DDI_HCOMMANDLIST hCL, D3D12DDI_HRESOURCE resource,
                                         const D3D12DDIARG_DISCARD_RESOURCE_0003 *args) {
    ID3D12GraphicsCommandList2_DiscardResource(CL(hCL), umd_res(resource), (const D3D12_DISCARD_REGION *)args);
}

/* Width and height of a format's compression block; the DDI's physical
 * footprints count blocks, the API's count texels. */
static void format_block(DXGI_FORMAT format, UINT *w, UINT *h) {
    *w = *h = 1;
    if ((format >= DXGI_FORMAT_BC1_TYPELESS && format <= DXGI_FORMAT_BC5_SNORM) ||
        (format >= DXGI_FORMAT_BC6H_TYPELESS && format <= DXGI_FORMAT_BC7_UNORM_SRGB))
        *w = *h = 4;
    else if (format == DXGI_FORMAT_R8G8_B8G8_UNORM || format == DXGI_FORMAT_G8R8_G8B8_UNORM ||
             format == DXGI_FORMAT_YUY2)
        *w = 2;
}

static bool copy_location(const D3D12DDIARG_BUFFER_PLACEMENT *placement, D3D12DDIARG_PLACED_RESOURCE layout,
                          D3D12_TEXTURE_COPY_LOCATION *location) {
    location->pResource = umd_res(placement->BaseAddress.UMD.hResource);
    switch (layout.Layout) {
    case D3D12DDI_RL_SELECT_SUBRESOURCE:
        location->Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        location->SubresourceIndex = (UINT)placement->BaseAddress.UMD.Offset;
        return true;
    case D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED: {
        const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT *p = layout.pLayout;
        UINT bw, bh;
        format_block(p->Format, &bw, &bh);
        location->Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        location->PlacedFootprint.Offset = placement->BaseAddress.UMD.Offset;
        location->PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT) {
            p->Format, p->PhysicalWidth * bw, p->PhysicalHeight * bh, p->PhysicalDepth, p->Pitch,
        };
        return true;
    }
    case D3D12DDI_RL_PLACED_VIRTUAL_SUBRESOURCE_PITCHED: {
        const D3D12DDIARG_VIRTUAL_SUBRESOURCE_PITCHED_LAYOUT *p = layout.pLayout;
        UINT bw, bh;
        format_block(p->Format, &bw, &bh);
        location->Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        location->PlacedFootprint.Offset = placement->BaseAddress.UMD.Offset;
        location->PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT) {
            p->Format, p->PhysicalWidth * bw, p->PhysicalHeight * bh, p->PhysicalDepth, p->Pitch,
        };
        return true;
    }
    default:
        ERROR("%s: layout %d", __FUNCTION__, layout.Layout);
        return false;
    }
}

static void APIENTRY cl_copy_texture_region(D3D12DDI_HCOMMANDLIST hCL, const D3D12DDIARG_BUFFER_PLACEMENT *dst,
                                            D3D12DDIARG_PLACED_RESOURCE dst_layout, UINT x, UINT y, UINT z,
                                            const D3D12DDIARG_BUFFER_PLACEMENT *src, D3D12DDIARG_PLACED_RESOURCE src_layout,
                                            const D3D12DDI_BOX *box) {
    D3D12_TEXTURE_COPY_LOCATION d, s;
    if (!copy_location(dst, dst_layout, &d) || !copy_location(src, src_layout, &s)) {
        list_error(umd_cl(hCL), E_INVALIDARG);
        return;
    }
    if (d3d12_umd_log_enabled()) {
        INFO("copy texture region: dst=%p+0x%llx type=%u fmt=%u off=%llu %ux%ux%u, "
             "src=%p+0x%llx type=%u fmt=%u off=%llu %ux%ux%u, at=%u,%u,%u box=%s",
             (void *)d.pResource, (unsigned long long)dst->BaseAddress.UMD.Offset, d.Type,
             d.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ? d.PlacedFootprint.Footprint.Format : 0,
             d.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ? (unsigned long long)d.PlacedFootprint.Offset :
                                                                    (unsigned long long)d.SubresourceIndex,
             d.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ? d.PlacedFootprint.Footprint.Width : 0,
             d.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ? d.PlacedFootprint.Footprint.Height : 0,
             d.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ? d.PlacedFootprint.Footprint.Depth : 0,
             (void *)s.pResource, (unsigned long long)src->BaseAddress.UMD.Offset, s.Type,
             s.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ? s.PlacedFootprint.Footprint.Format : 0,
             s.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ? (unsigned long long)s.PlacedFootprint.Offset :
                                                                    (unsigned long long)s.SubresourceIndex,
             s.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ? s.PlacedFootprint.Footprint.Width : 0,
             s.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ? s.PlacedFootprint.Footprint.Height : 0,
             s.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ? s.PlacedFootprint.Footprint.Depth : 0,
             x, y, z, box ? "yes" : "no");
        if (box)
            INFO("copy texture region box: %ld,%ld,%ld-%ld,%ld,%ld", box->Left, box->Top, box->Front,
                 box->Right, box->Bottom, box->Back);
    }
    ID3D12GraphicsCommandList2_CopyTextureRegion(CL(hCL), &d, x, y, z, &s, (const D3D12_BOX *)box);
}

static void APIENTRY cl_resource_copy(D3D12DDI_HCOMMANDLIST hCL, D3D12DDI_HRESOURCE dst, D3D12DDI_HRESOURCE src) {
    ID3D12GraphicsCommandList2_CopyResource(CL(hCL), umd_res(dst), umd_res(src));
}

static void APIENTRY cl_copy_buffer_region(D3D12DDI_HCOMMANDLIST hCL, D3D12DDIARG_BUFFER_PLACEMENT dst,
                                           D3D12DDIARG_BUFFER_PLACEMENT src, UINT64 size) {
    ID3D12GraphicsCommandList2_CopyBufferRegion(CL(hCL), umd_res(dst.BaseAddress.UMD.hResource), dst.BaseAddress.UMD.Offset,
                                                umd_res(src.BaseAddress.UMD.hResource), src.BaseAddress.UMD.Offset, size);
}

static void APIENTRY cl_resolve_subresource(D3D12DDI_HCOMMANDLIST hCL, D3D12DDI_HRESOURCE dst, UINT dst_sub,
                                            D3D12DDI_HRESOURCE src, UINT src_sub, DXGI_FORMAT format) {
    ID3D12GraphicsCommandList2_ResolveSubresource(CL(hCL), umd_res(dst), dst_sub, umd_res(src), src_sub, format);
}

static void APIENTRY cl_execute_bundle(D3D12DDI_HCOMMANDLIST hCL, D3D12DDI_HCOMMANDLIST bundle) {
    ID3D12GraphicsCommandList2_ExecuteBundle(CL(hCL), (ID3D12GraphicsCommandList *)CL(bundle));
}

static void APIENTRY cl_execute_indirect(D3D12DDI_HCOMMANDLIST hCL, D3D12DDI_HCOMMANDSIGNATURE signature, UINT max_count,
                                         D3D12DDIARG_BUFFER_PLACEMENT args, D3D12DDIARG_BUFFER_PLACEMENT count) {
    D3D12_UMD_CommandSignature *sig = signature.pDrvPrivate;
    ID3D12GraphicsCommandList2_ExecuteIndirect(CL(hCL), sig->sig, max_count, umd_res(args.BaseAddress.UMD.hResource),
                                               args.BaseAddress.UMD.Offset, umd_res(count.BaseAddress.UMD.hResource),
                                               count.BaseAddress.UMD.Offset);
}

static void APIENTRY cl_resource_barrier(D3D12DDI_HCOMMANDLIST hCL, UINT count, const D3D12DDIARG_RESOURCE_BARRIER_0022 *barriers) {
    D3D12_RESOURCE_BARRIER stack[16], *out = stack;
    if (count > ARRAY_SIZE(stack) && !(out = malloc(count * sizeof(*out)))) {
        list_error(umd_cl(hCL), E_OUTOFMEMORY);
        return;
    }

    UINT n = 0;
    for (UINT i = 0; i < count; i++) {
        const D3D12DDIARG_RESOURCE_BARRIER_0022 *b = &barriers[i];
        if (d3d12_umd_log_enabled())
            INFO("resource barrier[%u]: type=%u flags=0x%x resource=%p before=0x%x after=0x%x subresource=%u",
                 i, b->Type, b->Flags, (void *)b->Transition.hResource.pDrvPrivate,
                 b->Type == D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION ? b->Transition.StateBefore : 0,
                 b->Type == D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION ? b->Transition.StateAfter : 0,
                 b->Type == D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION ? b->Transition.Subresource : 0);
        D3D12_RESOURCE_BARRIER *o = &out[n];
        /* BEGIN_ONLY / END_ONLY share their values with the API. */
        o->Flags = (D3D12_RESOURCE_BARRIER_FLAGS)(b->Flags & (D3D12DDI_RESOURCE_BARRIER_FLAG_BEGIN_ONLY |
                                                             D3D12DDI_RESOURCE_BARRIER_FLAG_END_ONLY));
        switch (b->Type) {
        case D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION:
            o->Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            o->Transition = (D3D12_RESOURCE_TRANSITION_BARRIER) {
                umd_res(b->Transition.hResource), b->Transition.Subresource,
                (D3D12_RESOURCE_STATES)b->Transition.StateBefore, (D3D12_RESOURCE_STATES)b->Transition.StateAfter,
            };
            n++;
            break;
        case D3D12DDI_RESOURCE_BARRIER_TYPE_UAV:
            o->Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            o->UAV.pResource = umd_res(b->UAV.hResource);
            n++;
            break;
        case D3D12DDI_RESOURCE_BARRIER_TYPE_0022_RANGED:
            /* The API's aliasing barriers arrive as ranged barriers on the
             * resource coming into use. */
            if (!(b->Flags & D3D12DDI_RESOURCE_BARRIER_FLAG_0022_ALIASING))
                break;
            o->Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
            o->Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            o->Aliasing = (D3D12_RESOURCE_ALIASING_BARRIER) { NULL, umd_res(b->Ranged.hResource) };
            n++;
            break;
        default:
            WARN("%s: barrier type %d", __FUNCTION__, b->Type);
            break;
        }
    }
    if (n)
        ID3D12GraphicsCommandList2_ResourceBarrier(CL(hCL), n, out);
    if (out != stack)
        free(out);
}

static void APIENTRY cl_begin_query(D3D12DDI_HCOMMANDLIST hCL, D3D12DDI_HQUERYHEAP heap, D3D12DDI_QUERY_TYPE type, UINT index) {
    ID3D12GraphicsCommandList2_BeginQuery(CL(hCL), ((D3D12_UMD_QueryHeap *)heap.pDrvPrivate)->heap, (D3D12_QUERY_TYPE)type, index);
}

static void APIENTRY cl_end_query(D3D12DDI_HCOMMANDLIST hCL, D3D12DDI_HQUERYHEAP heap, D3D12DDI_QUERY_TYPE type, UINT index) {
    ID3D12GraphicsCommandList2_EndQuery(CL(hCL), ((D3D12_UMD_QueryHeap *)heap.pDrvPrivate)->heap, (D3D12_QUERY_TYPE)type, index);
}

static void APIENTRY cl_resolve_query_data(D3D12DDI_HCOMMANDLIST hCL, D3D12DDI_HQUERYHEAP heap, D3D12DDI_QUERY_TYPE type,
                                           UINT start, UINT count, D3D12DDI_HRESOURCE dst, UINT64 offset) {
    ID3D12GraphicsCommandList2_ResolveQueryData(CL(hCL), ((D3D12_UMD_QueryHeap *)heap.pDrvPrivate)->heap,
                                                (D3D12_QUERY_TYPE)type, start, count, umd_res(dst), offset);
}

static void APIENTRY cl_set_predication(D3D12DDI_HCOMMANDLIST hCL, D3D12DDI_HRESOURCE resource, UINT64 offset,
                                        D3D12DDI_PREDICATION_OP op) {
    ID3D12GraphicsCommandList2_SetPredication(CL(hCL), umd_res(resource), offset, (D3D12_PREDICATION_OP)op);
}

static void APIENTRY cl_ia_set_topology(D3D12DDI_HCOMMANDLIST hCL, D3D12DDI_PRIMITIVE_TOPOLOGY topology) {
    ID3D12GraphicsCommandList2_IASetPrimitiveTopology(CL(hCL), (D3D12_PRIMITIVE_TOPOLOGY)topology);
}

static void APIENTRY cl_rs_set_viewports(D3D12DDI_HCOMMANDLIST hCL, UINT count, const D3D12DDI_VIEWPORT *viewports) {
    ID3D12GraphicsCommandList2_RSSetViewports(CL(hCL), count, (const D3D12_VIEWPORT *)viewports);
}

static void APIENTRY cl_rs_set_scissor_rects(D3D12DDI_HCOMMANDLIST hCL, UINT count, const D3D12DDI_RECT *rects) {
    ID3D12GraphicsCommandList2_RSSetScissorRects(CL(hCL), count, (const D3D12_RECT *)rects);
}

static void APIENTRY cl_om_set_blend_factor(D3D12DDI_HCOMMANDLIST hCL, const FLOAT *factor) {
    ID3D12GraphicsCommandList2_OMSetBlendFactor(CL(hCL), factor);
}

static void APIENTRY cl_om_set_stencil_ref(D3D12DDI_HCOMMANDLIST hCL, UINT ref) {
    ID3D12GraphicsCommandList2_OMSetStencilRef(CL(hCL), ref);
}

static void APIENTRY cl_set_pipeline_state(D3D12DDI_HCOMMANDLIST hCL, D3D12DDI_HPIPELINESTATE handle) {
    D3D12_UMD_PipelineState *pso = handle.pDrvPrivate;
    /* The reset block sets the list's initial state, which may be none. */
    if (pso && pso->pso)
        ID3D12GraphicsCommandList2_SetPipelineState(CL(hCL), pso->pso);
}

static void APIENTRY cl_set_descriptor_heaps(D3D12DDI_HCOMMANDLIST hCL, UINT count, D3D12DDI_HDESCRIPTORHEAP *heaps) {
    ID3D12DescriptorHeap *vk[2];
    UINT n = 0;
    for (UINT i = 0; i < count && n < ARRAY_SIZE(vk); i++) {
        D3D12_UMD_DescriptorHeap *heap = heaps[i].pDrvPrivate;
        if (heap)
            vk[n++] = heap->heap;
    }
    ID3D12GraphicsCommandList2_SetDescriptorHeaps(CL(hCL), n, vk);
}

static void APIENTRY cl_set_compute_root_signature(D3D12DDI_HCOMMANDLIST hCL, D3D12DDI_HROOTSIGNATURE rs) {
    ID3D12GraphicsCommandList2_SetComputeRootSignature(CL(hCL), umd_root_signature(rs));
}

static void APIENTRY cl_set_graphics_root_signature(D3D12DDI_HCOMMANDLIST hCL, D3D12DDI_HROOTSIGNATURE rs) {
    ID3D12GraphicsCommandList2_SetGraphicsRootSignature(CL(hCL), umd_root_signature(rs));
}

static void APIENTRY cl_set_compute_root_descriptor_table(D3D12DDI_HCOMMANDLIST hCL, UINT index, D3D12DDI_GPU_DESCRIPTOR_HANDLE base) {
    ID3D12GraphicsCommandList2_SetComputeRootDescriptorTable(CL(hCL), index, gpu_handle(base));
}

static void APIENTRY cl_set_graphics_root_descriptor_table(D3D12DDI_HCOMMANDLIST hCL, UINT index, D3D12DDI_GPU_DESCRIPTOR_HANDLE base) {
    ID3D12GraphicsCommandList2_SetGraphicsRootDescriptorTable(CL(hCL), index, gpu_handle(base));
}

static void APIENTRY cl_set_compute_root_32bit_constant(D3D12DDI_HCOMMANDLIST hCL, UINT index, UINT data, UINT offset) {
    ID3D12GraphicsCommandList2_SetComputeRoot32BitConstant(CL(hCL), index, data, offset);
}

static void APIENTRY cl_set_graphics_root_32bit_constant(D3D12DDI_HCOMMANDLIST hCL, UINT index, UINT data, UINT offset) {
    ID3D12GraphicsCommandList2_SetGraphicsRoot32BitConstant(CL(hCL), index, data, offset);
}

static void APIENTRY cl_set_compute_root_32bit_constants(D3D12DDI_HCOMMANDLIST hCL, UINT index, UINT count, const void *data,
                                                         UINT offset) {
    ID3D12GraphicsCommandList2_SetComputeRoot32BitConstants(CL(hCL), index, count, data, offset);
}

static void APIENTRY cl_set_graphics_root_32bit_constants(D3D12DDI_HCOMMANDLIST hCL, UINT index, UINT count, const void *data,
                                                          UINT offset) {
    ID3D12GraphicsCommandList2_SetGraphicsRoot32BitConstants(CL(hCL), index, count, data, offset);
}

static void APIENTRY cl_set_compute_root_cbv(D3D12DDI_HCOMMANDLIST hCL, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) {
    ID3D12GraphicsCommandList2_SetComputeRootConstantBufferView(CL(hCL), index, va);
}

static void APIENTRY cl_set_graphics_root_cbv(D3D12DDI_HCOMMANDLIST hCL, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) {
    ID3D12GraphicsCommandList2_SetGraphicsRootConstantBufferView(CL(hCL), index, va);
}

static void APIENTRY cl_set_compute_root_srv(D3D12DDI_HCOMMANDLIST hCL, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) {
    ID3D12GraphicsCommandList2_SetComputeRootShaderResourceView(CL(hCL), index, va);
}

static void APIENTRY cl_set_graphics_root_srv(D3D12DDI_HCOMMANDLIST hCL, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) {
    ID3D12GraphicsCommandList2_SetGraphicsRootShaderResourceView(CL(hCL), index, va);
}

static void APIENTRY cl_set_compute_root_uav(D3D12DDI_HCOMMANDLIST hCL, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) {
    ID3D12GraphicsCommandList2_SetComputeRootUnorderedAccessView(CL(hCL), index, va);
}

static void APIENTRY cl_set_graphics_root_uav(D3D12DDI_HCOMMANDLIST hCL, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) {
    ID3D12GraphicsCommandList2_SetGraphicsRootUnorderedAccessView(CL(hCL), index, va);
}

static void APIENTRY cl_ia_set_index_buffer(D3D12DDI_HCOMMANDLIST hCL, const D3D12DDI_INDEX_BUFFER_VIEW *view) {
    ID3D12GraphicsCommandList2_IASetIndexBuffer(CL(hCL), (const D3D12_INDEX_BUFFER_VIEW *)view);
}

static void APIENTRY cl_ia_set_vertex_buffers(D3D12DDI_HCOMMANDLIST hCL, UINT start, UINT count,
                                              const D3D12DDI_VERTEX_BUFFER_VIEW *views) {
    ID3D12GraphicsCommandList2_IASetVertexBuffers(CL(hCL), start, count, (const D3D12_VERTEX_BUFFER_VIEW *)views);
}

static void APIENTRY cl_so_set_targets(D3D12DDI_HCOMMANDLIST hCL, UINT start, UINT count,
                                       const D3D12DDI_STREAM_OUTPUT_BUFFER_VIEW *views) {
    ID3D12GraphicsCommandList2_SOSetTargets(CL(hCL), start, count, (const D3D12_STREAM_OUTPUT_BUFFER_VIEW *)views);
}

static void APIENTRY cl_om_set_render_targets(D3D12DDI_HCOMMANDLIST hCL, UINT count, const D3D12DDI_CPU_DESCRIPTOR_HANDLE *rtvs,
                                              BOOL single_range, const D3D12DDI_CPU_DESCRIPTOR_HANDLE *dsv) {
    static_assert(sizeof(D3D12DDI_CPU_DESCRIPTOR_HANDLE) == sizeof(D3D12_CPU_DESCRIPTOR_HANDLE));
    ID3D12GraphicsCommandList2_OMSetRenderTargets(CL(hCL), count, (const D3D12_CPU_DESCRIPTOR_HANDLE *)rtvs, single_range,
                                                  (const D3D12_CPU_DESCRIPTOR_HANDLE *)dsv);
}

static void APIENTRY cl_set_marker(D3D12DDI_HCOMMANDLIST hCL, UINT64 marker) {
    (void)hCL;
    (void)marker;
}

/* UMD-only entry point in the statically linked vkd3d. The runtime may have
 * already bound the initial PSO, so ClearState(NULL) would erase valid state. */
extern void vkd3d_clear_root_arguments(ID3D12GraphicsCommandList *command_list);

static void APIENTRY cl_clear_root_arguments(D3D12DDI_HCOMMANDLIST hCL) {
    vkd3d_clear_root_arguments((ID3D12GraphicsCommandList *)CL(hCL));
}

static void APIENTRY cl_om_set_depth_bounds(D3D12DDI_HCOMMANDLIST hCL, FLOAT min, FLOAT max) {
    ID3D12GraphicsCommandList2_OMSetDepthBounds(CL(hCL), min, max);
}

static void APIENTRY cl_set_sample_positions(D3D12DDI_HCOMMANDLIST hCL, UINT samples, UINT pixels,
                                             D3D12DDI_SAMPLE_POSITION *positions) {
    ID3D12GraphicsCommandList2_SetSamplePositions(CL(hCL), samples, pixels, (D3D12_SAMPLE_POSITION *)positions);
}

static void APIENTRY cl_resolve_subresource_region(D3D12DDI_HCOMMANDLIST hCL, D3D12DDI_HRESOURCE dst, UINT dst_sub,
                                                   UINT dst_x, UINT dst_y, D3D12DDI_HRESOURCE src, UINT src_sub,
                                                   D3D12DDI_RECT *src_rect, DXGI_FORMAT format, D3D12DDI_RESOLVE_MODE mode) {
    ID3D12GraphicsCommandList2_ResolveSubresourceRegion(CL(hCL), umd_res(dst), dst_sub, dst_x, dst_y, umd_res(src), src_sub,
                                                        (D3D12_RECT *)src_rect, format, (D3D12_RESOLVE_MODE)mode);
}

static void APIENTRY cl_set_protected_resource_session(D3D12DDI_HCOMMANDLIST hCL, D3D12DDI_HPROTECTEDRESOURCESESSION_0030 session) {
    (void)hCL;
    (void)session;
}

static void APIENTRY cl_write_buffer_immediate(D3D12DDI_HCOMMANDLIST hCL, UINT count,
                                               const D3D12DDI_WRITEBUFFERIMMEDIATE_PARAMETER_0032 *params,
                                               const D3D12DDI_WRITEBUFFERIMMEDIATE_MODE_0032 *modes) {
    ID3D12GraphicsCommandList2_WriteBufferImmediate(CL(hCL), count, (const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *)params,
                                                    (const D3D12_WRITEBUFFERIMMEDIATE_MODE *)modes);
}

static void APIENTRY cl_set_view_instance_mask(D3D12DDI_HCOMMANDLIST hCL, UINT mask) {
    ID3D12GraphicsCommandList2_SetViewInstanceMask(CL(hCL), mask);
}

void d3d12_umd_install_cmdlist_core(D3D12DDI_DEVICE_FUNCS_CORE_0040 *t) {
    t->pfnCalcPrivateCommandPoolSize = calc_command_pool_size;
    t->pfnCreateCommandPool = create_command_pool;
    t->pfnDestroyCommandPool = destroy_command_pool;
    t->pfnResetCommandPool = reset_command_pool;
    t->pfnCalcPrivateCommandRecorderSize = calc_command_recorder_size;
    t->pfnCreateCommandRecorder = create_command_recorder;
    t->pfnDestroyCommandRecorder = destroy_command_recorder;
    t->pfnCommandRecorderSetCommandPoolAsTarget = command_recorder_set_command_pool_as_target;
    t->pfnCalcPrivateCommandListSize = calc_command_list_size;
    t->pfnCreateCommandList = create_command_list;
    t->pfnDestroyCommandList = destroy_command_list;
}

static void APIENTRY cl_present(D3D12DDI_HCOMMANDLIST hCL, D3D12DDI_HCOMMANDQUEUE hQueue,
                                const D3D12DDIARG_PRESENT_0001 *args, D3D12DDI_PRESENT_0028 *out) {
    D3D12_UMD_Device *device = umd_cl(hCL)->device;
    memset(out, 0, sizeof(*out));
    if (!args || args->SurfacesToPresent != 1 || !args->phSurfacesToPresent) {
        d3d12_umd_set_error(device, E_INVALIDARG);
        return;
    }
    D3D12_UMD_Resource *resource = umd_resource(args->phSurfacesToPresent[0].hSurface);
    /* 9On12's blt backbuffers are ordinary committed textures, not PRIMARY
     * heaps. Preserve their rendering storage and copy to a runtime-owned
     * linear allocation on the present command list. AddedGpuWork makes the
     * runtime submit that list before its kernel present. */
    if (resource && !resource->hKMAllocation) {
        if (args->phSurfacesToPresent[0].SubResourceIndex || args->hDstResource.pDrvPrivate ||
                resource->desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                resource->desc.SampleDesc.Count != 1 || resource->desc.DepthOrArraySize != 1 ||
                resource->desc.MipLevels != 1) {
            d3d12_umd_set_error(device, E_NOTIMPL);
            return;
        }
        if (!resource->present_shadow) {
            D3D12_UMD_Resource *shadow = calloc(1, sizeof(*shadow));
            if (!shadow) { d3d12_umd_set_error(device, E_OUTOFMEMORY); return; }
            shadow->desc = resource->desc;
            HRESULT hr = d3d12_umd_create_primary(device, shadow, resource->hRTResource);
            if (FAILED(hr)) {
                if (shadow->res) ID3D12Resource_Release(shadow->res);
                d3d12_umd_destroy_primary(device, shadow);
                free(shadow);
                d3d12_umd_set_error(device, hr);
                return;
            }
            resource->present_shadow = shadow;
        }
        D3D12_UMD_Resource *shadow = resource->present_shadow;
        D3D12_RESOURCE_BARRIER barriers[2] = {
            { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
              .Transition = { .pResource = resource->res, .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                  .StateBefore = D3D12_RESOURCE_STATE_PRESENT, .StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE } },
            { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
              .Transition = { .pResource = shadow->res, .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                  .StateBefore = D3D12_RESOURCE_STATE_PRESENT, .StateAfter = D3D12_RESOURCE_STATE_COPY_DEST } },
        };
        ID3D12GraphicsCommandList2_ResourceBarrier(umd_cl(hCL)->cl, 2, barriers);
        ID3D12GraphicsCommandList2_CopyResource(umd_cl(hCL)->cl, shadow->res, resource->res);
        for (UINT i = 0; i < 2; ++i) {
            barriers[i].Transition.StateBefore = barriers[i].Transition.StateAfter;
            barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        }
        ID3D12GraphicsCommandList2_ResourceBarrier(umd_cl(hCL)->cl, 2, barriers);
        out->AddedGpuWork = TRUE;
        umd_cl(hCL)->synchronous_present = true;
        resource = shadow;
    }
    /* ECL installed the GPU completion wait on this context before returning;
     * the runtime queues present after that wait. */
    HANDLE context = d3d12_umd_present_context(hQueue);
    if (!resource || !resource->hKMAllocation || !context) {
        ERROR("present: invalid queue or primary surface (resource=%p allocation=0x%x context=%p)",
              resource, resource ? resource->hKMAllocation : 0, context);
        d3d12_umd_set_error(device, E_INVALIDARG);
        return;
    }
    out->hSrcAllocation = resource->hKMAllocation;
    out->hContext = context;
    /* BroadcastContextCount counts additional contexts; this queue has one. */
    out->BroadcastSrcAllocation[0] = resource->hKMAllocation;
    out->BackBufferMultiplicity = 1;
}

/* Blt, CopyTiles and AtomicCopyBufferRegion stay generated stubs. */
void d3d12_umd_install_cmdlist(D3D12DDI_COMMAND_LIST_FUNCS_3D_0040 *t) {
    t->pfnPresent = cl_present;
    t->pfnCloseCommandList = cl_close;
    t->pfnResetCommandList = cl_reset;
    t->pfnDrawInstanced = cl_draw_instanced;
    t->pfnDrawIndexedInstanced = cl_draw_indexed_instanced;
    t->pfnDispatch = cl_dispatch;
    t->pfnClearUnorderedAccessViewUint = cl_clear_uav_uint;
    t->pfnClearUnorderedAccessViewFloat = cl_clear_uav_float;
    t->pfnClearRenderTargetView = cl_clear_rtv;
    t->pfnClearDepthStencilView = cl_clear_dsv;
    t->pfnDiscardResource = cl_discard_resource;
    t->pfnCopyTextureRegion = cl_copy_texture_region;
    t->pfnResourceCopy = cl_resource_copy;
    t->pfnCopyBufferRegion = cl_copy_buffer_region;
    t->pfnResourceResolveSubresource = cl_resolve_subresource;
    t->pfnExecuteBundle = cl_execute_bundle;
    t->pfnExecuteIndirect = cl_execute_indirect;
    t->pfnResourceBarrier = cl_resource_barrier;
    t->pfnBeginQuery = cl_begin_query;
    t->pfnEndQuery = cl_end_query;
    t->pfnResolveQueryData = cl_resolve_query_data;
    t->pfnSetPredication = cl_set_predication;
    t->pfnIaSetTopology = cl_ia_set_topology;
    t->pfnRsSetViewports = cl_rs_set_viewports;
    t->pfnRsSetScissorRects = cl_rs_set_scissor_rects;
    t->pfnOmSetBlendFactor = cl_om_set_blend_factor;
    t->pfnOmSetStencilRef = cl_om_set_stencil_ref;
    t->pfnSetPipelineState = cl_set_pipeline_state;
    t->pfnSetDescriptorHeaps = cl_set_descriptor_heaps;
    t->pfnSetComputeRootSignature = cl_set_compute_root_signature;
    t->pfnSetGraphicsRootSignature = cl_set_graphics_root_signature;
    t->pfnSetComputeRootDescriptorTable = cl_set_compute_root_descriptor_table;
    t->pfnSetGraphicsRootDescriptorTable = cl_set_graphics_root_descriptor_table;
    t->pfnSetComputeRoot32BitConstant = cl_set_compute_root_32bit_constant;
    t->pfnSetGraphicsRoot32BitConstant = cl_set_graphics_root_32bit_constant;
    t->pfnSetComputeRoot32BitConstants = cl_set_compute_root_32bit_constants;
    t->pfnSetGraphicsRoot32BitConstants = cl_set_graphics_root_32bit_constants;
    t->pfnSetComputeRootConstantBufferView = cl_set_compute_root_cbv;
    t->pfnSetGraphicsRootConstantBufferView = cl_set_graphics_root_cbv;
    t->pfnSetComputeRootShaderResourceView = cl_set_compute_root_srv;
    t->pfnSetGraphicsRootShaderResourceView = cl_set_graphics_root_srv;
    t->pfnSetComputeRootUnorderedAccessView = cl_set_compute_root_uav;
    t->pfnSetGraphicsRootUnorderedAccessView = cl_set_graphics_root_uav;
    t->pfnIASetIndexBuffer = cl_ia_set_index_buffer;
    t->pfnIASetVertexBuffers = cl_ia_set_vertex_buffers;
    t->pfnSOSetTargets = cl_so_set_targets;
    t->pfnOMSetRenderTargets = cl_om_set_render_targets;
    t->pfnSetMarker = cl_set_marker;
    t->pfnClearRootArguments = cl_clear_root_arguments;
    t->pfnOMSetDepthBounds = cl_om_set_depth_bounds;
    t->pfnSetSamplePositions = cl_set_sample_positions;
    t->pfnResourceResolveSubresourceRegion = cl_resolve_subresource_region;
    t->pfnSetProtectedResourceSession = cl_set_protected_resource_session;
    t->pfnWriteBufferImmediate = cl_write_buffer_immediate;
    t->pfnSetViewInstanceMask = cl_set_view_instance_mask;
}
