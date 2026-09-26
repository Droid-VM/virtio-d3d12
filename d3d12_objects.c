#include "d3d12_umd.h"

/* Device-level queries the runtime makes inside D3D12CreateDevice (formats,
 * node map, descriptor sizes), the extended-features table, and the assembly
 * of every DDI table from the generated stubs and each file's installer. */

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
    t->pfnGetDescriptorSizeInBytes = get_descriptor_size_in_bytes;
    t->pfnGetImplicitPhysicalAdapterMask = get_implicit_physical_adapter_mask;
    t->pfnGetPresentPrivateDriverDataSize = get_present_private_driver_data_size;
    t->pfnQueryNodeMap = query_node_map;

    d3d12_umd_install_pipeline_core(t);
    d3d12_umd_install_resource_core(t);
    d3d12_umd_install_descriptor_core(t);
    d3d12_umd_install_cmdlist_core(t);
    d3d12_umd_install_queue_core(t);
}

void d3d12_umd_fill_command_list(D3D12DDI_COMMAND_LIST_FUNCS_3D_0040 *t) {
    d3d12_umd_stub_fill_cl(t);
    d3d12_umd_install_cmdlist(t);
}

void d3d12_umd_fill_command_queue(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *t) {
    d3d12_umd_stub_fill_queue(t);
    d3d12_umd_install_queue(t);
}

void d3d12_umd_fill_extended_features(D3D12DDI_EXTENDED_FEATURES_FUNCS_0021 *t) {
    d3d12_umd_stub_fill_ext(t);
    t->pfnGetSupportedExtendedFeatures = get_supported_extended_features;
    t->pfnGetSupportedExtendedFeatureVersions = get_supported_extended_feature_versions;
    t->pfnEnableExtendedFeature = enable_extended_feature;
    t->pfnSetExtendedFeatureCallbacks = set_extended_feature_callbacks;
}
