#include "d3d12_umd.h"

/* The device-level slots the runtime drives inside D3D12CreateDevice: format
 * queries, node map, descriptor sizes, and the objects of its own internal
 * pipelines. Pipeline objects keep their DDI description for now; turning them
 * into vkd3d objects is the command-path work (dx12plan.md P2). */

/* ---------------------------------------------------------------------------
 * Formats.
 */

static UINT format_support_to_ddi(const D3D12_FEATURE_DATA_FORMAT_SUPPORT *s) {
    static const struct { UINT api, ddi; } s1[] = {
        { D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE, D3D12DDI_FORMAT_SUPPORT_SHADER_SAMPLE },
        { D3D12_FORMAT_SUPPORT1_RENDER_TARGET, D3D12DDI_FORMAT_SUPPORT_RENDERTARGET },
        { D3D12_FORMAT_SUPPORT1_BLENDABLE, D3D12DDI_FORMAT_SUPPORT_BLENDABLE },
        { D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET, D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET },
        { D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD, D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD },
        { D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER, D3D12DDI_FORMAT_SUPPORT_VERTEX_BUFFER },
        { D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW, D3D12DDI_FORMAT_SUPPORT_UAV_WRITES },
        { D3D12_FORMAT_SUPPORT1_BUFFER, D3D12DDI_FORMAT_SUPPORT_BUFFER },
        { D3D12_FORMAT_SUPPORT1_SHADER_GATHER, D3D12DDI_FORMAT_SUPPORT_SHADER_GATHER },
        { D3D12_FORMAT_SUPPORT1_DISPLAY, D3D12DDI_FORMAT_SUPPORT_DISPLAY },
    }, s2[] = {
        { D3D12_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP, D3D12DDI_FORMAT_SUPPORT_OUTPUT_MERGER_LOGIC_OP },
        { D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD, D3D12DDI_FORMAT_SUPPORT_UAV_READS },
        /* TILED: no tiled resources. */
    };
    UINT ddi = 0;
    for (size_t i = 0; i < ARRAY_SIZE(s1); i++)
        if (s->Support1 & s1[i].api)
            ddi |= s1[i].ddi;
    for (size_t i = 0; i < ARRAY_SIZE(s2); i++)
        if (s->Support2 & s2[i].api)
            ddi |= s2[i].ddi;
    return ddi;
}

/* vkd3d answers depth formats with the capabilities of the Vulkan depth format
 * behind them, render target and blending included. The runtime checks every
 * member of a typeless cast set against its own format table and fails device
 * creation on a bit the format cannot have, so trim to what D3D allows. */
static UINT depth_format_mask(DXGI_FORMAT format, const D3D12_FEATURE_DATA_FORMAT_SUPPORT *s) {
    switch (format) {
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        /* Shader-resource views of a depth/stencil plane only. */
        return D3D12DDI_FORMAT_SUPPORT_SHADER_SAMPLE | D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD |
               D3D12DDI_FORMAT_SUPPORT_SHADER_GATHER;
    default:
        if (s->Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL)
            return ~(UINT)(D3D12DDI_FORMAT_SUPPORT_RENDERTARGET | D3D12DDI_FORMAT_SUPPORT_BLENDABLE);
        return ~0u;
    }
}

static void APIENTRY check_format_support(D3D12DDI_HDEVICE hDevice, DXGI_FORMAT format, UINT *support) {
    D3D12_UMD_Device *device = umd_device(hDevice);
    D3D12_FEATURE_DATA_FORMAT_SUPPORT data = { .Format = format };
    HRESULT hr = ID3D12Device_CheckFeatureSupport(device->d3d12, D3D12_FEATURE_FORMAT_SUPPORT, &data, sizeof(data));
    *support = SUCCEEDED(hr) ? format_support_to_ddi(&data) & depth_format_mask(format, &data) : 0;
    if (format == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM && !*support)
        *support = D3D12DDI_FORMAT_SUPPORT_NOT_SUPPORTED;
    if (d3d12_umd_log_enabled())
        INFO("format %u: hr=0x%08lx api=0x%x/0x%x ddi=0x%x", format, hr, data.Support1, data.Support2, *support);
}

static void APIENTRY check_multisample_quality_levels(D3D12DDI_HDEVICE hDevice, DXGI_FORMAT format, UINT sample_count,
                                                      D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAGS flags, UINT *levels) {
    D3D12_UMD_Device *device = umd_device(hDevice);
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS data = {
        .Format = format,
        .SampleCount = sample_count,
        .Flags = (D3D12_MULTISAMPLE_QUALITY_LEVEL_FLAGS)flags,
    };
    HRESULT hr = ID3D12Device_CheckFeatureSupport(device->d3d12, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                                  &data, sizeof(data));
    *levels = SUCCEEDED(hr) ? data.NumQualityLevels : 0;
    /* A cast-set member without the MSRT bit must report no quality levels;
     * typeless parents keep theirs, the runtime compares members against them. */
    const D3D12_FEATURE_DATA_FORMAT_SUPPORT none = { .Format = format };
    if (!(depth_format_mask(format, &none) & D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET))
        *levels = 0;
    if (d3d12_umd_log_enabled())
        INFO("msaa format %u x%u flags=%u: hr=0x%08lx levels=%u", format, sample_count, flags, hr, *levels);
}

/* ---------------------------------------------------------------------------
 * Nodes and descriptors.
 */

static void APIENTRY query_node_map(D3D12DDI_HDEVICE hDevice, UINT count, UINT *map) {
    (void)hDevice;
    for (UINT i = 0; i < count; i++)
        map[i] = i;
}

static UINT APIENTRY get_implicit_physical_adapter_mask(D3D12DDI_HDEVICE hDevice) {
    (void)hDevice;
    return 1;
}

static UINT APIENTRY get_descriptor_size_in_bytes(D3D12DDI_HDEVICE hDevice, D3D12DDI_DESCRIPTOR_HEAP_TYPE type) {
    D3D12_UMD_Device *device = umd_device(hDevice);
    /* Same order as D3D12_DESCRIPTOR_HEAP_TYPE. */
    return (UINT)type < ARRAY_SIZE(device->descriptor_size) ? device->descriptor_size[type] : 0;
}

static UINT APIENTRY get_present_private_driver_data_size(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_PRESENT_0001 *args) {
    (void)hDevice;
    (void)args;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Pipeline sub-state: kept as described, consumed by CreatePipelineState.
 */

static SIZE_T APIENTRY calc_element_layout_size(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATEELEMENTLAYOUT_0010 *args) {
    (void)hDevice;
    return sizeof(D3D12_UMD_ElementLayout) + args->NumElements * sizeof(D3D12DDIARG_INPUT_ELEMENT_DESC);
}

static void APIENTRY create_element_layout(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATEELEMENTLAYOUT_0010 *args,
                                           D3D12DDI_HELEMENTLAYOUT handle) {
    (void)hDevice;
    D3D12_UMD_ElementLayout *layout = handle.pDrvPrivate;
    layout->num_elements = args->NumElements;
    layout->elements = (D3D12DDIARG_INPUT_ELEMENT_DESC *)(layout + 1);
    memcpy(layout->elements, args->pVertexElements, args->NumElements * sizeof(*layout->elements));
}

static void APIENTRY destroy_element_layout(D3D12DDI_HDEVICE hDevice, D3D12DDI_HELEMENTLAYOUT handle) {
    (void)hDevice;
    (void)handle;
}

static SIZE_T APIENTRY calc_blend_state_size(D3D12DDI_HDEVICE hDevice, const D3D12DDI_BLEND_DESC_0010 *desc) {
    (void)hDevice;
    (void)desc;
    return sizeof(D3D12_UMD_BlendState);
}

static void APIENTRY create_blend_state(D3D12DDI_HDEVICE hDevice, const D3D12DDI_BLEND_DESC_0010 *desc,
                                        D3D12DDI_HBLENDSTATE handle) {
    (void)hDevice;
    ((D3D12_UMD_BlendState *)handle.pDrvPrivate)->desc = *desc;
}

static void APIENTRY destroy_blend_state(D3D12DDI_HDEVICE hDevice, D3D12DDI_HBLENDSTATE handle) {
    (void)hDevice;
    (void)handle;
}

static SIZE_T APIENTRY calc_depth_stencil_state_size(D3D12DDI_HDEVICE hDevice, const D3D12DDI_DEPTH_STENCIL_DESC_0025 *desc) {
    (void)hDevice;
    (void)desc;
    return sizeof(D3D12_UMD_DepthStencilState);
}

static void APIENTRY create_depth_stencil_state(D3D12DDI_HDEVICE hDevice, const D3D12DDI_DEPTH_STENCIL_DESC_0025 *desc,
                                                D3D12DDI_HDEPTHSTENCILSTATE handle) {
    (void)hDevice;
    ((D3D12_UMD_DepthStencilState *)handle.pDrvPrivate)->desc = *desc;
}

static void APIENTRY destroy_depth_stencil_state(D3D12DDI_HDEVICE hDevice, D3D12DDI_HDEPTHSTENCILSTATE handle) {
    (void)hDevice;
    (void)handle;
}

static SIZE_T APIENTRY calc_rasterizer_state_size(D3D12DDI_HDEVICE hDevice, const D3D12DDI_RASTERIZER_DESC_0010 *desc) {
    (void)hDevice;
    (void)desc;
    return sizeof(D3D12_UMD_RasterizerState);
}

static void APIENTRY create_rasterizer_state(D3D12DDI_HDEVICE hDevice, const D3D12DDI_RASTERIZER_DESC_0010 *desc,
                                             D3D12DDI_HRASTERIZERSTATE handle) {
    (void)hDevice;
    ((D3D12_UMD_RasterizerState *)handle.pDrvPrivate)->desc = *desc;
}

static void APIENTRY destroy_rasterizer_state(D3D12DDI_HDEVICE hDevice, D3D12DDI_HRASTERIZERSTATE handle) {
    (void)hDevice;
    (void)handle;
}

/* ---------------------------------------------------------------------------
 * Shaders: the runtime passes the DXIL program part, headed by a program
 * token and its length in DWORDs (never a DXBC container). Keep a copy.
 */

static SIZE_T shader_code_dwords(const UINT *code) {
    return code ? code[1] : 0;
}

static SIZE_T APIENTRY calc_shader_size(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_SHADER_0026 *args) {
    (void)hDevice;
    return sizeof(D3D12_UMD_Shader) + shader_code_dwords(args->pShaderCode) * sizeof(UINT);
}

static void create_shader(const D3D12DDIARG_CREATE_SHADER_0026 *args, D3D12DDI_HSHADER handle) {
    D3D12_UMD_Shader *shader = handle.pDrvPrivate;
    shader->code_dwords = shader_code_dwords(args->pShaderCode);
    shader->code = (UINT *)(shader + 1);
    memcpy(shader->code, args->pShaderCode, shader->code_dwords * sizeof(UINT));
    shader->root_signature = args->hRootSignature;
    INFO("create shader %p: token 0x%08x, %zu dwords", shader, shader->code_dwords ? shader->code[0] : 0,
         (size_t)shader->code_dwords);
}

/* One body for every stage: the stage is in the program token. */
static void APIENTRY create_stage_shader(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_SHADER_0026 *args,
                                         D3D12DDI_HSHADER handle) {
    (void)hDevice;
    create_shader(args, handle);
}

static SIZE_T APIENTRY calc_geometry_shader_with_stream_output_size(
        D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT_0026 *args) {
    return calc_shader_size(hDevice, &args->CreateShader);
}

static void APIENTRY create_geometry_shader_with_stream_output(
        D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT_0026 *args,
        D3D12DDI_HSHADER handle) {
    (void)hDevice;
    create_shader(&args->CreateShader, handle);
}

static void APIENTRY destroy_shader(D3D12DDI_HDEVICE hDevice, D3D12DDI_HSHADER handle) {
    (void)hDevice;
    (void)handle;
}

/* ---------------------------------------------------------------------------
 * Root signatures and pipeline states.
 */

static SIZE_T APIENTRY calc_root_signature_size(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013 *args) {
    (void)hDevice;
    (void)args;
    return sizeof(D3D12_UMD_RootSignature);
}

static HRESULT APIENTRY create_root_signature(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013 *args,
                                              D3D12DDI_HROOTSIGNATURE handle) {
    (void)hDevice;
    D3D12_UMD_RootSignature *rs = handle.pDrvPrivate;
    const D3D12DDI_ROOT_SIGNATURE_0013 *desc = args->pRootSignature_1_1;
    rs->version = args->Version;
    rs->num_parameters = desc ? desc->NumParameters : 0;
    rs->num_static_samplers = desc ? desc->NumStaticSamplers : 0;
    rs->flags = desc ? desc->Flags : 0;
    INFO("create root signature %p: version %d, %u parameters, %u static samplers, flags 0x%x", rs, rs->version,
         rs->num_parameters, rs->num_static_samplers, rs->flags);
    return S_OK;
}

static void APIENTRY destroy_root_signature(D3D12DDI_HDEVICE hDevice, D3D12DDI_HROOTSIGNATURE handle) {
    (void)hDevice;
    (void)handle;
}

static SIZE_T APIENTRY calc_pipeline_state_size(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_PIPELINE_STATE_0033 *args) {
    (void)hDevice;
    (void)args;
    return sizeof(D3D12_UMD_PipelineState);
}

static HRESULT APIENTRY create_pipeline_state(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_PIPELINE_STATE_0033 *args,
                                              D3D12DDI_HPIPELINESTATE handle, D3D12DDI_HRTPIPELINESTATE rt) {
    (void)hDevice;
    (void)rt;
    D3D12_UMD_PipelineState *pso = handle.pDrvPrivate;
    pso->desc = *args;
    pso->compute = args->hComputeShader.pDrvPrivate != NULL;
    INFO("create pipeline state %p: %s, %u render targets", pso, pso->compute ? "compute" : "graphics",
         args->NumRenderTargets);
    return S_OK;
}

static void APIENTRY destroy_pipeline_state(D3D12DDI_HDEVICE hDevice, D3D12DDI_HPIPELINESTATE handle) {
    (void)hDevice;
    (void)handle;
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

/* ---------------------------------------------------------------------------
 * Command pools and recorders (the 0040 split of the command allocator).
 */

static SIZE_T APIENTRY calc_command_pool_size(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_COMMAND_POOL_0040 *args) {
    (void)hDevice;
    (void)args;
    return sizeof(D3D12_UMD_CommandPool);
}

static HRESULT APIENTRY create_command_pool(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_COMMAND_POOL_0040 *args,
                                            D3D12DDI_HCOMMANDPOOL_0040 handle) {
    (void)hDevice;
    ((D3D12_UMD_CommandPool *)handle.pDrvPrivate)->flags = args->PoolFlags;
    return S_OK;
}

static void APIENTRY destroy_command_pool(D3D12DDI_HDEVICE hDevice, D3D12DDI_HCOMMANDPOOL_0040 handle) {
    (void)hDevice;
    (void)handle;
}

static void APIENTRY reset_command_pool(D3D12DDI_HDEVICE hDevice, D3D12DDI_HCOMMANDPOOL_0040 handle) {
    (void)hDevice;
    (void)handle;
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
    recorder->pool.pDrvPrivate = NULL;
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
    ((D3D12_UMD_CommandRecorder *)recorder.pDrvPrivate)->pool = pool;
}

/* ---------------------------------------------------------------------------
 * Extended features: none offered.
 */

static HRESULT APIENTRY get_supported_extended_features(D3D12DDI_HDEVICE hDevice, UINT32 *count,
                                                        D3D12DDI_FEATURE_0020 *features) {
    (void)hDevice;
    (void)features;
    *count = 0;
    return S_OK;
}

static HRESULT APIENTRY get_supported_extended_feature_versions(D3D12DDI_HDEVICE hDevice, D3D12DDI_FEATURE_0020 feature,
                                                                UINT32 *count, UINT32 *versions) {
    (void)hDevice;
    (void)feature;
    (void)versions;
    *count = 0;
    return E_INVALIDARG;
}

static HRESULT APIENTRY enable_extended_feature(D3D12DDI_HDEVICE hDevice, D3D12DDI_FEATURE_0020 feature, UINT32 version) {
    (void)hDevice;
    INFO("%s: feature %d version %u refused", __FUNCTION__, feature, version);
    return E_INVALIDARG;
}

static HRESULT APIENTRY set_extended_feature_callbacks(D3D12DDI_HDEVICE hDevice, D3D12DDI_TABLE_TYPE table,
                                                       const void *callbacks, SIZE_T size) {
    (void)hDevice;
    (void)callbacks;
    INFO("%s: table %d size %zu ignored", __FUNCTION__, table, (size_t)size);
    return S_OK;
}

/* ---------------------------------------------------------------------------
 * Tables: every slot starts as a named stub (ddi_stubs.c), then the
 * implemented ones are installed over it.
 */

void d3d12_umd_fill_core(D3D12DDI_DEVICE_FUNCS_CORE_0040 *t) {
    d3d12_umd_stub_fill_core(t);
    t->pfnCheckFormatSupport = check_format_support;
    t->pfnCheckMultisampleQualityLevels = check_multisample_quality_levels;

    t->pfnCalcPrivateElementLayoutSize = calc_element_layout_size;
    t->pfnCreateElementLayout = create_element_layout;
    t->pfnDestroyElementLayout = destroy_element_layout;
    t->pfnCalcPrivateBlendStateSize = calc_blend_state_size;
    t->pfnCreateBlendState = create_blend_state;
    t->pfnDestroyBlendState = destroy_blend_state;
    t->pfnCalcPrivateDepthStencilStateSize = calc_depth_stencil_state_size;
    t->pfnCreateDepthStencilState = create_depth_stencil_state;
    t->pfnDestroyDepthStencilState = destroy_depth_stencil_state;
    t->pfnCalcPrivateRasterizerStateSize = calc_rasterizer_state_size;
    t->pfnCreateRasterizerState = create_rasterizer_state;
    t->pfnDestroyRasterizerState = destroy_rasterizer_state;

    t->pfnCalcPrivateShaderSize = calc_shader_size;
    t->pfnCreateVertexShader = create_stage_shader;
    t->pfnCreatePixelShader = create_stage_shader;
    t->pfnCreateGeometryShader = create_stage_shader;
    t->pfnCreateComputeShader = create_stage_shader;
    t->pfnCalcPrivateGeometryShaderWithStreamOutput = calc_geometry_shader_with_stream_output_size;
    t->pfnCreateGeometryShaderWithStreamOutput = create_geometry_shader_with_stream_output;
    t->pfnCalcPrivateTessellationShaderSize = calc_shader_size;
    t->pfnCreateHullShader = create_stage_shader;
    t->pfnCreateDomainShader = create_stage_shader;
    t->pfnDestroyShader = destroy_shader;

    t->pfnCalcPrivateCommandPoolSize = calc_command_pool_size;
    t->pfnCreateCommandPool = create_command_pool;
    t->pfnDestroyCommandPool = destroy_command_pool;
    t->pfnResetCommandPool = reset_command_pool;

    t->pfnCalcPrivatePipelineStateSize = calc_pipeline_state_size;
    t->pfnCreatePipelineState = create_pipeline_state;
    t->pfnDestroyPipelineState = destroy_pipeline_state;

    t->pfnGetDescriptorSizeInBytes = get_descriptor_size_in_bytes;
    t->pfnCalcPrivateRootSignatureSize = calc_root_signature_size;
    t->pfnCreateRootSignature = create_root_signature;
    t->pfnDestroyRootSignature = destroy_root_signature;

    t->pfnMakeResident = make_resident;
    t->pfnEvict = evict;

    t->pfnGetImplicitPhysicalAdapterMask = get_implicit_physical_adapter_mask;
    t->pfnGetPresentPrivateDriverDataSize = get_present_private_driver_data_size;
    t->pfnQueryNodeMap = query_node_map;

    t->pfnCalcPrivateCommandRecorderSize = calc_command_recorder_size;
    t->pfnCreateCommandRecorder = create_command_recorder;
    t->pfnDestroyCommandRecorder = destroy_command_recorder;
    t->pfnCommandRecorderSetCommandPoolAsTarget = command_recorder_set_command_pool_as_target;
}

void d3d12_umd_fill_command_list(D3D12DDI_COMMAND_LIST_FUNCS_3D_0040 *t) {
    d3d12_umd_stub_fill_cl(t);
}

void d3d12_umd_fill_command_queue(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *t) {
    d3d12_umd_stub_fill_queue(t);
}

void d3d12_umd_fill_extended_features(D3D12DDI_EXTENDED_FEATURES_FUNCS_0021 *t) {
    d3d12_umd_stub_fill_ext(t);
    t->pfnGetSupportedExtendedFeatures = get_supported_extended_features;
    t->pfnGetSupportedExtendedFeatureVersions = get_supported_extended_feature_versions;
    t->pfnEnableExtendedFeature = enable_extended_feature;
    t->pfnSetExtendedFeatureCallbacks = set_extended_feature_callbacks;
}
