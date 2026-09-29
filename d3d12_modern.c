#include "d3d12_umd.h"

/* R8/0098 enables independent runtime devices. Keep the implementation shared
 * with R4, but translate changed argument layouts and assign table slots by
 * name: the newer tables are not binary-compatible prefixes of R4. */
static INIT_ONCE legacy_once = INIT_ONCE_STATIC_INIT;
static D3D12DDI_DEVICE_FUNCS_CORE_0040 legacy_core;
static D3D12DDI_COMMAND_LIST_FUNCS_3D_0040 legacy_cl;
static D3D12DDI_EXTENDED_FEATURES_FUNCS_0021 legacy_ext;

static BOOL CALLBACK init_legacy(INIT_ONCE *once, void *arg, void **context) {
    (void)once; (void)arg; (void)context;
    d3d12_umd_fill_core(&legacy_core);
    d3d12_umd_fill_command_list(&legacy_cl);
    d3d12_umd_fill_extended_features(&legacy_ext);
    return TRUE;
}

static D3D12DDI_DEPTH_STENCIL_DESC_0025 depth_desc(const D3D12DDI_DEPTH_STENCIL_DESC_0095 *d) {
    return (D3D12DDI_DEPTH_STENCIL_DESC_0025) {
        .DepthEnable = d->DepthEnable, .DepthWriteMask = d->DepthWriteMask, .DepthFunc = d->DepthFunc,
        .StencilEnable = d->StencilEnable, .FrontEnable = d->FrontEnable, .BackEnable = d->BackEnable,
        .StencilReadMask = d->FrontEnable ? d->FrontFaceStencilReadMask : d->BackFaceStencilReadMask,
        .StencilWriteMask = d->FrontEnable ? d->FrontFaceStencilWriteMask : d->BackFaceStencilWriteMask,
        .FrontFace = d->FrontFace, .BackFace = d->BackFace,
        .LibraryReference = d->LibraryReference, .DepthBoundsTestEnable = d->DepthBoundsTestEnable,
    };
}
static SIZE_T APIENTRY calc_depth(D3D12DDI_HDEVICE h, const D3D12DDI_DEPTH_STENCIL_DESC_0095 *d) {
    D3D12DDI_DEPTH_STENCIL_DESC_0025 old = depth_desc(d);
    return legacy_core.pfnCalcPrivateDepthStencilStateSize(h, &old);
}
static void APIENTRY create_depth(D3D12DDI_HDEVICE h, const D3D12DDI_DEPTH_STENCIL_DESC_0095 *d,
                                  D3D12DDI_HDEPTHSTENCILSTATE state) {
    D3D12DDI_DEPTH_STENCIL_DESC_0025 old = depth_desc(d);
    legacy_core.pfnCreateDepthStencilState(h, &old, state);
    D3D12_UMD_DepthStencilState *stored = state.pDrvPrivate;
    stored->back_read_mask = d->BackFaceStencilReadMask;
    stored->back_write_mask = d->BackFaceStencilWriteMask;
}

static D3D12DDIARG_CREATECOMMANDQUEUE_0023 queue_desc(const D3D12DDIARG_CREATECOMMANDQUEUE_0050 *d) {
    return (D3D12DDIARG_CREATECOMMANDQUEUE_0023) {
        .QueueFlags = d->QueueFlags, .NodeMask = d->NodeMask, .QueueCreationFlags = d->QueueCreationFlags,
    };
}
static SIZE_T APIENTRY calc_queue(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATECOMMANDQUEUE_0050 *d) {
    D3D12DDIARG_CREATECOMMANDQUEUE_0023 old = queue_desc(d);
    return legacy_core.pfnCalcPrivateCommandQueueSize(h, &old);
}
static HRESULT APIENTRY create_queue(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATECOMMANDQUEUE_0050 *d,
                                     D3D12DDI_HCOMMANDQUEUE q, D3D12DDI_HRTCOMMANDQUEUE rt) {
    /* Hardware scheduling groups are not advertised. */
    if (d->SchedulingGroup.pDrvPrivate)
        return E_NOTIMPL;
    D3D12DDIARG_CREATECOMMANDQUEUE_0023 old = queue_desc(d);
    return legacy_core.pfnCreateCommandQueue(h, &old, q, rt);
}

static D3D12DDIARG_CREATE_PIPELINE_STATE_0033 pipeline_desc(const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 *d) {
    D3D12DDIARG_CREATE_PIPELINE_STATE_0033 old;
    /* 0075 appends mesh/amplification handles to the 0033 layout. */
    static_assert(offsetof(D3D12DDIARG_CREATE_PIPELINE_STATE_0075, hMeshShader) == sizeof(old));
    memcpy(&old, d, sizeof(old));
    return old;
}
static SIZE_T APIENTRY calc_pipeline(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 *d) {
    D3D12DDIARG_CREATE_PIPELINE_STATE_0033 old = pipeline_desc(d);
    return legacy_core.pfnCalcPrivatePipelineStateSize(h, &old);
}
static HRESULT APIENTRY create_pipeline(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 *d,
                                        D3D12DDI_HPIPELINESTATE p, D3D12DDI_HRTPIPELINESTATE rt) {
    if (d->hMeshShader.pDrvPrivate || d->hAmplificationShader.pDrvPrivate)
        return E_NOTIMPL;
    D3D12DDIARG_CREATE_PIPELINE_STATE_0033 old = pipeline_desc(d);
    return legacy_core.pfnCreatePipelineState(h, &old, p, rt);
}

static void APIENTRY create_sampler(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_SAMPLER_0096 *d,
                                    D3D12DDI_CPU_DESCRIPTOR_HANDLE dest) {
    const D3D12DDI_SAMPLER_DESC_0096 *s = d->pSamplerDesc;
    D3D12_UMD_Device *device = umd_device(h);
    ID3D12Device11 *device11;
    HRESULT hr = ID3D12Device_QueryInterface(device->d3d12, &IID_ID3D12Device11, (void **)&device11);
    if (FAILED(hr)) {
        d3d12_umd_set_error(device, hr);
        return;
    }
    static_assert(sizeof(*s) == sizeof(D3D12_SAMPLER_DESC2));
    static_assert(offsetof(D3D12DDI_SAMPLER_DESC_0096, Flags) == offsetof(D3D12_SAMPLER_DESC2, Flags));
    D3D12_SAMPLER_DESC2 desc;
    memcpy(&desc, s, sizeof(desc));
    ID3D12Device11_CreateSampler2(device11, &desc, (D3D12_CPU_DESCRIPTOR_HANDLE){dest.ptr});
    ID3D12Device11_Release(device11);
}

/* Enhanced barriers and relaxed format casting remain unadvertised. Translate
 * the initial layouts used by the runtime's legacy-resource API lowering. */
static D3D12DDI_RESOURCE_STATES initial_state(D3D12DDI_BARRIER_LAYOUT layout) {
    switch (layout) {
    case D3D12DDI_BARRIER_LAYOUT_GENERIC_READ:
    case D3D12DDI_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ:
    case D3D12DDI_BARRIER_LAYOUT_COMPUTE_QUEUE_GENERIC_READ:
    case D3D12DDI_BARRIER_LAYOUT_LEGACY_DIRECT_QUEUE_GENERIC_READ_COMPUTE_QUEUE_ACCESSIBLE:
        return (D3D12DDI_RESOURCE_STATES)D3D12_RESOURCE_STATE_GENERIC_READ;
    case D3D12DDI_BARRIER_LAYOUT_RENDER_TARGET: return (D3D12DDI_RESOURCE_STATES)D3D12_RESOURCE_STATE_RENDER_TARGET;
    case D3D12DDI_BARRIER_LAYOUT_UNORDERED_ACCESS:
    case D3D12DDI_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS:
    case D3D12DDI_BARRIER_LAYOUT_COMPUTE_QUEUE_UNORDERED_ACCESS:
        return (D3D12DDI_RESOURCE_STATES)D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case D3D12DDI_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE: return (D3D12DDI_RESOURCE_STATES)D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case D3D12DDI_BARRIER_LAYOUT_DEPTH_STENCIL_READ: return (D3D12DDI_RESOURCE_STATES)D3D12_RESOURCE_STATE_DEPTH_READ;
    case D3D12DDI_BARRIER_LAYOUT_SHADER_RESOURCE:
    case D3D12DDI_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE:
    case D3D12DDI_BARRIER_LAYOUT_COMPUTE_QUEUE_SHADER_RESOURCE:
    case D3D12DDI_BARRIER_LAYOUT_LEGACY_SHADER_RESOURCE:
        return (D3D12DDI_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    case D3D12DDI_BARRIER_LAYOUT_LEGACY_PIXEL_SHADER_RESOURCE:
        return (D3D12DDI_RESOURCE_STATES)D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case D3D12DDI_BARRIER_LAYOUT_COPY_SOURCE:
    case D3D12DDI_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_SOURCE:
    case D3D12DDI_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_SOURCE:
    case D3D12DDI_BARRIER_LAYOUT_LEGACY_COPY_SOURCE: return (D3D12DDI_RESOURCE_STATES)D3D12_RESOURCE_STATE_COPY_SOURCE;
    case D3D12DDI_BARRIER_LAYOUT_COPY_DEST:
    case D3D12DDI_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_DEST:
    case D3D12DDI_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_DEST:
    case D3D12DDI_BARRIER_LAYOUT_LEGACY_COPY_DEST: return (D3D12DDI_RESOURCE_STATES)D3D12_RESOURCE_STATE_COPY_DEST;
    case D3D12DDI_BARRIER_LAYOUT_RESOLVE_SOURCE: return (D3D12DDI_RESOURCE_STATES)D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    case D3D12DDI_BARRIER_LAYOUT_RESOLVE_DEST: return (D3D12DDI_RESOURCE_STATES)D3D12_RESOURCE_STATE_RESOLVE_DEST;
    default: return (D3D12DDI_RESOURCE_STATES)D3D12_RESOURCE_STATE_COMMON;
    }
}
static D3D12DDIARG_CREATERESOURCE_0003 resource_desc(const D3D12DDIARG_CREATERESOURCE_0088 *d) {
    return (D3D12DDIARG_CREATERESOURCE_0003) {
        .ReuseBufferGPUVA = d->ReuseBufferGPUVA, .ResourceType = d->ResourceType,
        .Width = d->Width, .Height = d->Height, .DepthOrArraySize = d->DepthOrArraySize,
        .MipLevels = d->MipLevels, .Format = d->Format, .SampleDesc = d->SampleDesc,
        .Layout = d->Layout, .Flags = d->Flags, .InitialResourceState = initial_state(d->InitialBarrierLayout),
        .pRowMajorLayout = d->pRowMajorLayout,
    };
}
static D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY calc_resource(D3D12DDI_HDEVICE h,
        const D3D12DDIARG_CREATEHEAP_0001 *heap, const D3D12DDIARG_CREATERESOURCE_0088 *res,
        D3D12DDI_HPROTECTEDRESOURCESESSION_0030 session) {
    D3D12DDIARG_CREATERESOURCE_0003 old;
    if (res) old = resource_desc(res);
    return legacy_core.pfnCalcPrivateHeapAndResourceSizes(h, heap, res ? &old : NULL, session);
}
static HRESULT APIENTRY create_resource(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATEHEAP_0001 *heap,
        D3D12DDI_HHEAP hh, D3D12DDI_HRTRESOURCE rt, const D3D12DDIARG_CREATERESOURCE_0088 *res,
        const D3D12DDI_CLEAR_VALUES *clear, D3D12DDI_HPROTECTEDRESOURCESESSION_0030 session, D3D12DDI_HRESOURCE hr) {
    D3D12DDIARG_CREATERESOURCE_0003 old;
    if (res) {
        if (res->NumCastableFormats || res->SamplerFeedbackMipRegion.Width ||
            res->SamplerFeedbackMipRegion.Height || res->SamplerFeedbackMipRegion.Depth)
            return E_NOTIMPL;
        old = resource_desc(res);
    }
    return legacy_core.pfnCreateHeapAndResource(h, heap, hh, rt, res ? &old : NULL, clear, session, hr);
}
static void APIENTRY check_resource(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATERESOURCE_0088 *res,
        D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS flags, UINT32 alignment, UINT nodes,
        D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 *out) {
    D3D12DDIARG_CREATERESOURCE_0003 old = resource_desc(res);
    legacy_core.pfnCheckResourceAllocationInfo(h, &old, flags, alignment, nodes, out);
}
static D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY calc_open(D3D12DDI_HDEVICE h,
        const D3D12DDIARG_OPENHEAP_0003 *args, D3D12DDI_HPROTECTEDRESOURCESESSION_0030 session) {
    (void)session;
    return legacy_core.pfnCalcPrivateOpenedHeapAndResourceSizes(h, args);
}
static HRESULT APIENTRY open_resource(D3D12DDI_HDEVICE h, const D3D12DDIARG_OPENHEAP_0003 *args,
        D3D12DDI_HHEAP heap, D3D12DDI_HRTRESOURCE rt, D3D12DDI_HPROTECTEDRESOURCESESSION_0030 session,
        D3D12DDI_HRESOURCE res) {
    if (session.pDrvPrivate) return E_NOTIMPL;
    return legacy_core.pfnOpenHeapAndResource(h, args, heap, rt, res);
}
static void APIENTRY present(D3D12DDI_HCOMMANDLIST list, D3D12DDI_HCOMMANDQUEUE queue,
        const D3D12DDIARG_PRESENT_0001 *args, D3D12DDI_PRESENT_0051 *out,
        D3D12DDI_PRESENT_CONTEXTS_0051 *contexts, D3D12DDI_PRESENT_HWQUEUES_0051 *hwqueues) {
    D3D12DDI_PRESENT_0028 old = {0};
    legacy_cl.pfnPresent(list, queue, args, &old);
    memset(out, 0, sizeof(*out));
    out->BroadcastSrcAllocation[0] = old.hSrcAllocation;
    out->BroadcastDstAllocation[0] = old.hDstAllocation;
    memcpy(out->BroadcastSrcAllocation + 1, old.BroadcastSrcAllocation, sizeof(old.BroadcastSrcAllocation));
    memcpy(out->BroadcastDstAllocation + 1, old.BroadcastDstAllocation, sizeof(old.BroadcastDstAllocation));
    out->AddedGpuWork = old.AddedGpuWork;
    out->BackBufferMultiplicity = old.BackBufferMultiplicity;
    out->SyncIntervalOverrideValid = old.SyncIntervalOverrideValid;
    out->SyncIntervalOverride = old.SyncIntervalOverride;
    if (contexts) {
        contexts->hContext = old.hContext;
        contexts->BroadcastContextCount = old.BroadcastContextCount;
        memcpy(contexts->BroadcastContext, old.BroadcastContext, sizeof(old.BroadcastContext));
    }
    if (hwqueues) memset(hwqueues, 0, sizeof(*hwqueues));
}
static HRESULT APIENTRY supported_features(D3D12DDI_HDEVICE h, D3D12DDI_FEATURE_0020 highest,
                                           UINT32 *count, D3D12DDI_FEATURE_0020 *features) {
    (void)highest;
    return legacy_ext.pfnGetSupportedExtendedFeatures(h, count, features);
}
static void APIENTRY background_processing(D3D12DDI_HDEVICE h, D3D12DDI_BACKGROUND_PROCESSING_MODE_0062 mode,
        D3D12DDI_MEASUREMENTS_ACTION_0062 action, BOOL *further) {
    (void)h; (void)mode; (void)action;
    *further = FALSE;
}

static HRESULT APIENTRY enumerate_meta_commands(D3D12DDI_HDEVICE h, UINT *count,
        D3D12DDIARG_META_COMMAND_DESC *descs) {
    (void)h; (void)descs;
    *count = 0;
    return S_OK;
}
static HRESULT APIENTRY enumerate_meta_parameters(D3D12DDI_HDEVICE h, GUID id,
        D3D12DDI_META_COMMAND_PARAMETER_STAGE stage, UINT *count,
        D3D12DDIARG_META_COMMAND_PARAMETER_DESC *descs) {
    (void)h; (void)id; (void)stage; (void)count; (void)descs;
    return E_INVALIDARG;
}
static void APIENTRY stencil_ref(D3D12DDI_HCOMMANDLIST h, UINT front, UINT back) {
    ID3D12GraphicsCommandList8 *cl8;
    HRESULT hr = ID3D12GraphicsCommandList2_QueryInterface(umd_cl(h)->cl,
            &IID_ID3D12GraphicsCommandList8, (void **)&cl8);
    if (SUCCEEDED(hr)) {
        ID3D12GraphicsCommandList8_OMSetFrontAndBackStencilRef(cl8, front, back);
        ID3D12GraphicsCommandList8_Release(cl8);
    } else {
        D3D12_UMD_CommandList *list = umd_cl(h);
        list->device->um->pfnSetCommandListErrorCb(list->hRT, hr);
    }
}

void d3d12_umd_fill_modern_core(D3D12DDI_DEVICE_FUNCS_CORE_0096 *t) {
    InitOnceExecuteOnce(&legacy_once, init_legacy, NULL, NULL);
    memset(t, 0, sizeof(*t));
    t->pfnCheckFormatSupport = legacy_core.pfnCheckFormatSupport;
    t->pfnCheckMultisampleQualityLevels = legacy_core.pfnCheckMultisampleQualityLevels;
    t->pfnGetMipPacking = legacy_core.pfnGetMipPacking;
    t->pfnCalcPrivateElementLayoutSize = legacy_core.pfnCalcPrivateElementLayoutSize;
    t->pfnCreateElementLayout = legacy_core.pfnCreateElementLayout;
    t->pfnDestroyElementLayout = legacy_core.pfnDestroyElementLayout;
    t->pfnCalcPrivateBlendStateSize = legacy_core.pfnCalcPrivateBlendStateSize;
    t->pfnCreateBlendState = legacy_core.pfnCreateBlendState;
    t->pfnDestroyBlendState = legacy_core.pfnDestroyBlendState;
    t->pfnCalcPrivateDepthStencilStateSize = calc_depth;
    t->pfnCreateDepthStencilState = create_depth;
    t->pfnDestroyDepthStencilState = legacy_core.pfnDestroyDepthStencilState;
    t->pfnCalcPrivateRasterizerStateSize = legacy_core.pfnCalcPrivateRasterizerStateSize;
    t->pfnCreateRasterizerState = legacy_core.pfnCreateRasterizerState;
    t->pfnDestroyRasterizerState = legacy_core.pfnDestroyRasterizerState;
    t->pfnCalcPrivateShaderSize = legacy_core.pfnCalcPrivateShaderSize;
    t->pfnCreateVertexShader = legacy_core.pfnCreateVertexShader;
    t->pfnCreatePixelShader = legacy_core.pfnCreatePixelShader;
    t->pfnCreateGeometryShader = legacy_core.pfnCreateGeometryShader;
    t->pfnCreateComputeShader = legacy_core.pfnCreateComputeShader;
    t->pfnCalcPrivateGeometryShaderWithStreamOutput = legacy_core.pfnCalcPrivateGeometryShaderWithStreamOutput;
    t->pfnCreateGeometryShaderWithStreamOutput = legacy_core.pfnCreateGeometryShaderWithStreamOutput;
    t->pfnCalcPrivateTessellationShaderSize = legacy_core.pfnCalcPrivateTessellationShaderSize;
    t->pfnCreateHullShader = legacy_core.pfnCreateHullShader;
    t->pfnCreateDomainShader = legacy_core.pfnCreateDomainShader;
    t->pfnDestroyShader = legacy_core.pfnDestroyShader;
    t->pfnCalcPrivateCommandQueueSize = calc_queue;
    t->pfnCreateCommandQueue = create_queue;
    t->pfnDestroyCommandQueue = legacy_core.pfnDestroyCommandQueue;
    t->pfnCalcPrivateCommandPoolSize = legacy_core.pfnCalcPrivateCommandPoolSize;
    t->pfnCreateCommandPool = legacy_core.pfnCreateCommandPool;
    t->pfnDestroyCommandPool = legacy_core.pfnDestroyCommandPool;
    t->pfnResetCommandPool = legacy_core.pfnResetCommandPool;
    t->pfnCalcPrivatePipelineStateSize = calc_pipeline;
    t->pfnCreatePipelineState = create_pipeline;
    t->pfnDestroyPipelineState = legacy_core.pfnDestroyPipelineState;
    t->pfnCalcPrivateCommandListSize = legacy_core.pfnCalcPrivateCommandListSize;
    t->pfnCreateCommandList = legacy_core.pfnCreateCommandList;
    t->pfnDestroyCommandList = legacy_core.pfnDestroyCommandList;
    t->pfnCalcPrivateFenceSize = legacy_core.pfnCalcPrivateFenceSize;
    t->pfnCreateFence = legacy_core.pfnCreateFence;
    t->pfnDestroyFence = legacy_core.pfnDestroyFence;
    t->pfnCalcPrivateDescriptorHeapSize = legacy_core.pfnCalcPrivateDescriptorHeapSize;
    t->pfnCreateDescriptorHeap = legacy_core.pfnCreateDescriptorHeap;
    t->pfnDestroyDescriptorHeap = legacy_core.pfnDestroyDescriptorHeap;
    t->pfnGetDescriptorSizeInBytes = legacy_core.pfnGetDescriptorSizeInBytes;
    t->pfnGetCPUDescriptorHandleForHeapStart = legacy_core.pfnGetCPUDescriptorHandleForHeapStart;
    t->pfnGetGPUDescriptorHandleForHeapStart = legacy_core.pfnGetGPUDescriptorHandleForHeapStart;
    t->pfnCreateShaderResourceView = legacy_core.pfnCreateShaderResourceView;
    t->pfnCreateConstantBufferView = legacy_core.pfnCreateConstantBufferView;
    t->pfnCreateSampler = create_sampler;
    t->pfnCreateUnorderedAccessView = legacy_core.pfnCreateUnorderedAccessView;
    t->pfnCreateRenderTargetView = legacy_core.pfnCreateRenderTargetView;
    t->pfnCreateDepthStencilView = legacy_core.pfnCreateDepthStencilView;
    t->pfnCalcPrivateRootSignatureSize = legacy_core.pfnCalcPrivateRootSignatureSize;
    t->pfnCreateRootSignature = legacy_core.pfnCreateRootSignature;
    t->pfnDestroyRootSignature = legacy_core.pfnDestroyRootSignature;
    t->pfnMapHeap = legacy_core.pfnMapHeap;
    t->pfnUnmapHeap = legacy_core.pfnUnmapHeap;
    t->pfnCalcPrivateHeapAndResourceSizes = calc_resource;
    t->pfnCreateHeapAndResource = create_resource;
    t->pfnDestroyHeapAndResource = legacy_core.pfnDestroyHeapAndResource;
    t->pfnMakeResident = legacy_core.pfnMakeResident;
    t->pfnEvict = legacy_core.pfnEvict;
    t->pfnCalcPrivateOpenedHeapAndResourceSizes = calc_open;
    t->pfnOpenHeapAndResource = open_resource;
    t->pfnCopyDescriptors = legacy_core.pfnCopyDescriptors;
    t->pfnCopyDescriptorsSimple = legacy_core.pfnCopyDescriptorsSimple;
    t->pfnCalcPrivateQueryHeapSize = legacy_core.pfnCalcPrivateQueryHeapSize;
    t->pfnCreateQueryHeap = legacy_core.pfnCreateQueryHeap;
    t->pfnDestroyQueryHeap = legacy_core.pfnDestroyQueryHeap;
    t->pfnCalcPrivateCommandSignatureSize = legacy_core.pfnCalcPrivateCommandSignatureSize;
    t->pfnCreateCommandSignature = legacy_core.pfnCreateCommandSignature;
    t->pfnDestroyCommandSignature = legacy_core.pfnDestroyCommandSignature;
    t->pfnCheckResourceVirtualAddress = legacy_core.pfnCheckResourceVirtualAddress;
    t->pfnCheckResourceAllocationInfo = check_resource;
    t->pfnCheckSubresourceInfo = legacy_core.pfnCheckSubresourceInfo;
    t->pfnCheckExistingResourceAllocationInfo = legacy_core.pfnCheckExistingResourceAllocationInfo;
    t->pfnOfferResources = legacy_core.pfnOfferResources;
    t->pfnReclaimResources = legacy_core.pfnReclaimResources;
    t->pfnGetImplicitPhysicalAdapterMask = legacy_core.pfnGetImplicitPhysicalAdapterMask;
    t->pfnGetPresentPrivateDriverDataSize = legacy_core.pfnGetPresentPrivateDriverDataSize;
    t->pfnQueryNodeMap = legacy_core.pfnQueryNodeMap;
    t->pfnRetrieveShaderComment = legacy_core.pfnRetrieveShaderComment;
    t->pfnCheckResourceAllocationHandle = legacy_core.pfnCheckResourceAllocationHandle;
    t->pfnCalcPrivatePipelineLibrarySize = legacy_core.pfnCalcPrivatePipelineLibrarySize;
    t->pfnCreatePipelineLibrary = legacy_core.pfnCreatePipelineLibrary;
    t->pfnDestroyPipelineLibrary = legacy_core.pfnDestroyPipelineLibrary;
    t->pfnAddPipelineStateToLibrary = legacy_core.pfnAddPipelineStateToLibrary;
    t->pfnCalcSerializedLibrarySize = legacy_core.pfnCalcSerializedLibrarySize;
    t->pfnSerializeLibrary = legacy_core.pfnSerializeLibrary;
    t->pfnGetDebugAllocationInfo = legacy_core.pfnGetDebugAllocationInfo;
    t->pfnCalcPrivateCommandRecorderSize = legacy_core.pfnCalcPrivateCommandRecorderSize;
    t->pfnCreateCommandRecorder = legacy_core.pfnCreateCommandRecorder;
    t->pfnDestroyCommandRecorder = legacy_core.pfnDestroyCommandRecorder;
    t->pfnCommandRecorderSetCommandPoolAsTarget = legacy_core.pfnCommandRecorderSetCommandPoolAsTarget;
    t->pfnSetBackgroundProcessingMode = background_processing;
    t->pfnEnumerateMetaCommands = enumerate_meta_commands;
    t->pfnEnumerateMetaCommandParameters = enumerate_meta_parameters;
}

void d3d12_umd_fill_modern_command_list(D3D12DDI_COMMAND_LIST_FUNCS_3D_0095 *t) {
    InitOnceExecuteOnce(&legacy_once, init_legacy, NULL, NULL);
    memset(t, 0, sizeof(*t));
    t->pfnCloseCommandList = legacy_cl.pfnCloseCommandList;
    t->pfnResetCommandList = legacy_cl.pfnResetCommandList;
    t->pfnDrawInstanced = legacy_cl.pfnDrawInstanced;
    t->pfnDrawIndexedInstanced = legacy_cl.pfnDrawIndexedInstanced;
    t->pfnDispatch = legacy_cl.pfnDispatch;
    t->pfnClearUnorderedAccessViewUint = legacy_cl.pfnClearUnorderedAccessViewUint;
    t->pfnClearUnorderedAccessViewFloat = legacy_cl.pfnClearUnorderedAccessViewFloat;
    t->pfnClearRenderTargetView = legacy_cl.pfnClearRenderTargetView;
    t->pfnClearDepthStencilView = legacy_cl.pfnClearDepthStencilView;
    t->pfnDiscardResource = legacy_cl.pfnDiscardResource;
    t->pfnCopyTextureRegion = legacy_cl.pfnCopyTextureRegion;
    t->pfnResourceCopy = legacy_cl.pfnResourceCopy;
    t->pfnCopyTiles = legacy_cl.pfnCopyTiles;
    t->pfnCopyBufferRegion = legacy_cl.pfnCopyBufferRegion;
    t->pfnResourceResolveSubresource = legacy_cl.pfnResourceResolveSubresource;
    t->pfnExecuteBundle = legacy_cl.pfnExecuteBundle;
    t->pfnExecuteIndirect = legacy_cl.pfnExecuteIndirect;
    t->pfnResourceBarrier = legacy_cl.pfnResourceBarrier;
    t->pfnBlt = legacy_cl.pfnBlt;
    t->pfnPresent = present;
    t->pfnBeginQuery = legacy_cl.pfnBeginQuery;
    t->pfnEndQuery = legacy_cl.pfnEndQuery;
    t->pfnResolveQueryData = legacy_cl.pfnResolveQueryData;
    t->pfnSetPredication = legacy_cl.pfnSetPredication;
    t->pfnIaSetTopology = legacy_cl.pfnIaSetTopology;
    t->pfnRsSetViewports = legacy_cl.pfnRsSetViewports;
    t->pfnRsSetScissorRects = legacy_cl.pfnRsSetScissorRects;
    t->pfnOmSetBlendFactor = legacy_cl.pfnOmSetBlendFactor;
    t->pfnOmSetStencilRef = legacy_cl.pfnOmSetStencilRef;
    t->pfnOmSetFrontAndBackStencilRef = stencil_ref;
    t->pfnSetPipelineState = legacy_cl.pfnSetPipelineState;
    t->pfnSetDescriptorHeaps = legacy_cl.pfnSetDescriptorHeaps;
    t->pfnSetComputeRootSignature = legacy_cl.pfnSetComputeRootSignature;
    t->pfnSetGraphicsRootSignature = legacy_cl.pfnSetGraphicsRootSignature;
    t->pfnSetComputeRootDescriptorTable = legacy_cl.pfnSetComputeRootDescriptorTable;
    t->pfnSetGraphicsRootDescriptorTable = legacy_cl.pfnSetGraphicsRootDescriptorTable;
    t->pfnSetComputeRoot32BitConstant = legacy_cl.pfnSetComputeRoot32BitConstant;
    t->pfnSetGraphicsRoot32BitConstant = legacy_cl.pfnSetGraphicsRoot32BitConstant;
    t->pfnSetComputeRoot32BitConstants = legacy_cl.pfnSetComputeRoot32BitConstants;
    t->pfnSetGraphicsRoot32BitConstants = legacy_cl.pfnSetGraphicsRoot32BitConstants;
    t->pfnSetComputeRootConstantBufferView = legacy_cl.pfnSetComputeRootConstantBufferView;
    t->pfnSetGraphicsRootConstantBufferView = legacy_cl.pfnSetGraphicsRootConstantBufferView;
    t->pfnSetComputeRootShaderResourceView = legacy_cl.pfnSetComputeRootShaderResourceView;
    t->pfnSetGraphicsRootShaderResourceView = legacy_cl.pfnSetGraphicsRootShaderResourceView;
    t->pfnSetComputeRootUnorderedAccessView = legacy_cl.pfnSetComputeRootUnorderedAccessView;
    t->pfnSetGraphicsRootUnorderedAccessView = legacy_cl.pfnSetGraphicsRootUnorderedAccessView;
    t->pfnIASetIndexBuffer = legacy_cl.pfnIASetIndexBuffer;
    t->pfnIASetVertexBuffers = legacy_cl.pfnIASetVertexBuffers;
    t->pfnSOSetTargets = legacy_cl.pfnSOSetTargets;
    t->pfnOMSetRenderTargets = legacy_cl.pfnOMSetRenderTargets;
    t->pfnSetMarker = legacy_cl.pfnSetMarker;
    t->pfnClearRootArguments = legacy_cl.pfnClearRootArguments;
    t->pfnAtomicCopyBufferRegion = legacy_cl.pfnAtomicCopyBufferRegion;
    t->pfnOMSetDepthBounds = legacy_cl.pfnOMSetDepthBounds;
    t->pfnSetSamplePositions = legacy_cl.pfnSetSamplePositions;
    t->pfnResourceResolveSubresourceRegion = legacy_cl.pfnResourceResolveSubresourceRegion;
    t->pfnSetProtectedResourceSession = legacy_cl.pfnSetProtectedResourceSession;
    t->pfnWriteBufferImmediate = legacy_cl.pfnWriteBufferImmediate;
    t->pfnSetViewInstanceMask = legacy_cl.pfnSetViewInstanceMask;
}

void d3d12_umd_fill_modern_extended_features(D3D12DDI_EXTENDED_FEATURES_FUNCS_0096 *t) {
    InitOnceExecuteOnce(&legacy_once, init_legacy, NULL, NULL);
    memset(t, 0, sizeof(*t));
    t->pfnGetSupportedExtendedFeatures = supported_features;
    t->pfnGetSupportedExtendedFeatureVersions = legacy_ext.pfnGetSupportedExtendedFeatureVersions;
    t->pfnEnableExtendedFeature = legacy_ext.pfnEnableExtendedFeature;
    t->pfnSetExtendedFeatureCallbacks = legacy_ext.pfnSetExtendedFeatureCallbacks;
}

