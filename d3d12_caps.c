#include "d3d12_umd.h"

/* Caps are answered before any device exists (3DPIPELINESUPPORT even before
 * version negotiation), so for now they are a fixed table for this GPU
 * (Adreno 830 through turnip + vkd3d-proton), pinned to FL 11_1 / SM 6.0.
 * Every caps struct only ever grew at the end, so each answer is built in its
 * newest layout and the runtime gets the prefix it asked for. */

D3D12DDI_3DPIPELINELEVEL d3d12_umd_pipeline_level(const D3D12_UMD_Adapter *adapter) {
    switch (adapter->feature_level_override) {
    case D3D_FEATURE_LEVEL_11_0: return D3D12DDI_3DPIPELINELEVEL_11_0;
    case D3D_FEATURE_LEVEL_12_0: return D3D12DDI_3DPIPELINELEVEL_12_0;
    default: return D3D12DDI_3DPIPELINELEVEL_11_1;
    }
}

static bool fl12(const D3D12_UMD_Adapter *adapter) {
    return d3d12_umd_pipeline_level(adapter) >= D3D12DDI_3DPIPELINELEVEL_12_0;
}

static HRESULT answer(const D3D12DDIARG_GETCAPS *args, const void *data, size_t size) {
    size_t n = args->DataSize < size ? args->DataSize : size;
    memcpy(args->pData, data, n);
    if (args->DataSize > size)
        memset((char *)args->pData + size, 0, args->DataSize - size);
    return S_OK;
}

static void options(const D3D12_UMD_Adapter *adapter, D3D12DDI_D3D12_OPTIONS_DATA_0089 *o) {
    memset(o, 0, sizeof(*o));
    o->ResourceBindingTier = D3D12DDI_RESOURCE_BINDING_TIER_3;
    o->ConservativeRasterizationTier = D3D12DDI_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED;
    /* No sparse binding on the virtio kernel backend; FL 12_0 would need 2. */
    o->TiledResourcesTier = D3D12DDI_TILED_RESOURCES_TIER_NOT_SUPPORTED;
    o->CrossNodeSharingTier = D3D12DDI_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
    o->VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation = TRUE;
    o->OutputMergerLogicOp = TRUE;
    o->ResourceHeapTier = D3D12DDI_RESOURCE_HEAP_TIER_2;
    o->DepthBoundsTestSupported = TRUE;
    /* DDI 0041 exposes this field; it allows the runtime's 64 KiB-aligned
     * MSAA placement workaround used by vkd3d-proton's tests. */
    o->ReservedBufferPlacementSupported = TRUE;
    /* Tier 1 unlocks the runtime rule "MSAA resources may be created in
     * RESOLVE_DEST only with programmable-MSAA tier 1" (the exact message
     * lives in D3D12Core.dll). Sample positions themselves are forwarded to
     * vkd3d's SetSamplePositions, which is a FIXME stub: programming them
     * for real needs VK_EXT_sample_locations in the inner device. */
    o->ProgrammableSamplePositionsTier = D3D12DDI_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_1;
    o->WriteBufferImmediateQueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D | D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE |
                                        D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    /* Barycentrics and view instancing need SM 6.1; render passes are not a
     * 0040 table; everything after is SM 6.3+ or enhanced barriers. */
    (void)adapter;
}

static void shader_caps(const D3D12_UMD_Adapter *adapter, D3D12DDI_SHADER_CAPS_0084 *s) {
    memset(s, 0, sizeof(*s));
    /* The backend supports min16float lowering even without native FP16 ops. */
    s->MinPrecision = D3D12DDI_SHADER_MIN_PRECISION_16_BIT;
    s->TypedUAVLoadAdditionalFormats = fl12(adapter);
    s->WaveOps = TRUE;
    s->WaveLaneCountMin = 64;
    s->WaveLaneCountMax = 128;
    s->TotalLaneCount = 2048;
    /* Native16BitOps needs SM 6.2, the 64-bit atomics SM 6.6. */
}

static const char *caps_name(D3D12DDICAPS_TYPE type) {
    switch (type) {
#define CASE(x) case D3D12DDICAPS_TYPE_##x: return #x;
    CASE(TEXTURE_LAYOUT) CASE(SWIZZLE_PATTERN) CASE(MEMORY_ARCHITECTURE) CASE(TEXTURE_LAYOUT_SETS)
    CASE(SHADER) CASE(ARCHITECTURE_INFO) CASE(D3D12_OPTIONS) CASE(3DPIPELINESUPPORT) CASE(GPUVA_CAPS)
    CASE(TEXTURE_LAYOUT1) CASE(0011_SHADER_MODELS) CASE(OPTIONS1_0103)
    CASE(0030_PROTECTED_RESOURCE_SESSION_SUPPORT) CASE(0022_CPU_PAGE_TABLE_FALSE_POSITIVES)
    CASE(0022_TEXTURE_LAYOUT) CASE(0022_SWIZZLE_PATTERN) CASE(0023_UMD_BASED_COMMAND_QUEUE_PRIORITY)
    CASE(0033_ADAPTER_COMPUTE_ONLY) CASE(0050_HARDWARE_SCHEDULING_CAPS) CASE(EXECUTECOMMANDLISTS_PARALLELISM)
    CASE(SAMPLER_FEEDBACK_0073) CASE(0073_SUPPORT_BATCHED_MARKERS) CASE(0081_3DPIPELINESUPPORT1)
    CASE(OPTIONS_0090) CASE(OPTIONS_0091) CASE(OPTIONS_0093) CASE(OPTIONS_0098) CASE(OPTIONS_0101)
    CASE(OPTIONS_0102) CASE(OPTIONS_0109) CASE(OPTIONS_0110) CASE(SHADER_MODEL_6_8_OPTIONS_0110)
#undef CASE
    default: return "?";
    }
}

static HRESULT get_caps(const D3D12_UMD_Adapter *adapter, const D3D12DDIARG_GETCAPS *args) {
    switch ((int)args->Type) {
    case D3D12DDICAPS_TYPE_3DPIPELINESUPPORT: {
        /* Never above 12_1 here; that is what 3DPIPELINESUPPORT1 is for. */
        D3D12DDI_3DPIPELINELEVEL level = d3d12_umd_pipeline_level(adapter);
        return answer(args, &level, sizeof(level));
    }
    case D3D12DDICAPS_TYPE_0081_3DPIPELINESUPPORT1: {
        /* In: the highest level the runtime understands. Out: ours, capped. */
        if (args->DataSize < sizeof(D3D12DDI_3DPIPELINESUPPORT1_DATA_0081))
            return E_INVALIDARG;
        D3D12DDI_3DPIPELINESUPPORT1_DATA_0081 *data = args->pData;
        D3D12DDI_3DPIPELINELEVEL level = d3d12_umd_pipeline_level(adapter);
        if (data->HighestRuntimeSupportedFeatureLevel < level)
            level = data->HighestRuntimeSupportedFeatureLevel;
        data->MaximumDriverSupportedFeatureLevel = level;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_D3D12_OPTIONS: {
        D3D12DDI_D3D12_OPTIONS_DATA_0089 o;
        options(adapter, &o);
        return answer(args, &o, sizeof(o));
    }
    case D3D12DDICAPS_TYPE_SHADER: {
        D3D12DDI_SHADER_CAPS_0084 s;
        shader_caps(adapter, &s);
        return answer(args, &s, sizeof(s));
    }
    case D3D12DDICAPS_TYPE_0011_SHADER_MODELS: {
        /* Gapless and starting at 5.1, or the runtime refuses the device. */
        static const D3D12DDI_SHADER_MODEL models[] = {
            D3D12DDI_SHADER_MODEL_5_1_RELEASE_0011,
            D3D12DDI_SHADER_MODEL_6_0_RELEASE_0011,
            D3D12DDI_SHADER_MODEL_6_1_RELEASE_0033,
            D3D12DDI_SHADER_MODEL_6_2_RELEASE_0042,
        };
        if (args->DataSize < sizeof(D3D12DDI_D3D12_SHADER_MODELS_DATA_0011))
            return E_INVALIDARG;
        const D3D12DDI_D3D12_SHADER_MODELS_DATA_0011 *data = args->pData;
        if (!data->pNumShaderModelsSupported)
            return E_INVALIDARG;
        if (data->pShaderModelsSupported) {
            UINT n = *data->pNumShaderModelsSupported;
            memcpy(data->pShaderModelsSupported, models, (n < ARRAY_SIZE(models) ? n : ARRAY_SIZE(models)) * sizeof(*models));
        }
        *data->pNumShaderModelsSupported = ARRAY_SIZE(models);
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_ARCHITECTURE_INFO: {
        D3D12DDI_ARCHITECTURE_INFO_DATA a = { .TileBasedDeferredRenderer = TRUE };
        return answer(args, &a, sizeof(a));
    }
    case D3D12DDICAPS_TYPE_MEMORY_ARCHITECTURE: {
        /* One pool of system memory, and uncached CPU mappings for the GPU. */
        D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041 m = {
            .UMA = TRUE,
            .IOCoherent = TRUE,
            .CacheCoherent = FALSE,
        };
        return answer(args, &m, sizeof(m));
    }
    case D3D12DDICAPS_TYPE_GPUVA_CAPS: {
        D3D12DDI_GPUVA_CAPS_0004 va = { .MaxGPUVirtualAddressBitsPerResource = 40 };
        return answer(args, &va, sizeof(va));
    }
    case D3D12DDICAPS_TYPE_TEXTURE_LAYOUT:
    case D3D12DDICAPS_TYPE_TEXTURE_LAYOUT1:
    case D3D12DDICAPS_TYPE_0022_TEXTURE_LAYOUT: {
        /* No device-dependent layouts, no standard swizzle, no row-major
         * textures: the layout is chosen by turnip and the host. */
        if (args->pInfo)
            return E_INVALIDARG;
        D3D12DDI_TEXTURE_LAYOUT_CAPS_0026 t = { 0 };
        return answer(args, &t, sizeof(t));
    }
    case D3D12DDICAPS_TYPE_TEXTURE_LAYOUT_SETS: {
        /* Enumerated as {ROW_MAJOR, functional unit}; the runtime stops at
         * the first failure. */
        const UINT *info = args->pInfo;
        if (!info || info[0] != D3D12DDI_TL_ROW_MAJOR || info[1] != D3D12DDI_FUNCUNIT_COMBINED)
            return E_UNEXPECTED;
        D3D12DDI_ROW_MAJOR_LAYOUT_CAPS r = { 0 };
        for (int i = 0; i < 2; i++)
            r.SubCaps[i] = (D3D12DDI_ROW_MAJOR_LAYOUT_SUB_CAPS) {
                .MaxElementSize = 16,
                /* The runtime caps these at 512 / 256 / 256 (the API's
                 * placement and row-pitch alignments) and fails device creation above. */
                .BaseOffsetAlignment = 512,
                .PitchAlignment = 256,
                .DepthPitchAlignment = 256,
            };
        return answer(args, &r, sizeof(r));
    }
    case D3D12DDICAPS_TYPE_0022_CPU_PAGE_TABLE_FALSE_POSITIVES: {
        D3D12DDI_COMMAND_QUEUE_FLAGS none = D3D12DDI_COMMAND_QUEUE_FLAG_NONE;
        return answer(args, &none, sizeof(none));
    }
    case D3D12DDICAPS_TYPE_0023_UMD_BASED_COMMAND_QUEUE_PRIORITY: {
        D3D12DDICAPS_UMD_BASED_COMMAND_QUEUE_PRIORITY_DATA_0023 p = { 0 };
        return answer(args, &p, sizeof(p));
    }
    case D3D12DDICAPS_TYPE_0050_HARDWARE_SCHEDULING_CAPS: {
        /* 0: no scheduling groups (the KMD has no hardware queues). */
        D3D12DDICAPS_HARDWARE_SCHEDULING_CAPS_0050 s = { 0 };
        return answer(args, &s, sizeof(s));
    }
    case D3D12DDICAPS_TYPE_0033_ADAPTER_COMPUTE_ONLY:
    case D3D12DDICAPS_TYPE_EXECUTECOMMANDLISTS_PARALLELISM:
    case D3D12DDICAPS_TYPE_0073_SUPPORT_BATCHED_MARKERS: {
        BOOL no = FALSE;
        return answer(args, &no, sizeof(no));
    }
    case D3D12DDICAPS_TYPE_OPTIONS1_0103: {
        D3D12DDI_OPTIONS_DATA_0103 o = { .WorkGraphsTier = D3D12DDI_WORK_GRAPHS_TIER_NOT_SUPPORTED };
        return answer(args, &o, sizeof(o));
    }
    case D3D12DDICAPS_TYPE_OPTIONS_0090:
    case D3D12DDICAPS_TYPE_OPTIONS_0091:
    case D3D12DDICAPS_TYPE_OPTIONS_0093:
    case D3D12DDICAPS_TYPE_OPTIONS_0098:
    case D3D12DDICAPS_TYPE_OPTIONS_0101:
    case D3D12DDICAPS_TYPE_OPTIONS_0109:
    case D3D12DDICAPS_TYPE_SHADER_MODEL_6_8_OPTIONS_0110:
    case D3D12DDICAPS_TYPE_SAMPLER_FEEDBACK_0073:
    case D3D12DDICAPS_TYPE_0030_PROTECTED_RESOURCE_SESSION_SUPPORT:
    case D3D12DDICAPS_TYPE_0074_PROTECTED_RESOURCE_SESSION_TYPE_COUNT:
    case D3D12DDICAPS_TYPE_0103_WAVE_MMA: {
        /* Newer than 0040 or not offered: every member FALSE / tier 0, which
         * only makes the runtime validate more strictly. */
        static const UINT zero[8];
        return answer(args, zero, sizeof(zero));
    }
    case D3D12DDICAPS_TYPE_OPTIONS_0110: {
        /* The lowest legal tier is 1_0 (10), not 0. */
        D3D12DDI_OPTIONS_DATA_0110 o = { .ExecuteIndirectTier = D3D12DDI_EXECUTE_INDIRECT_TIER_1_0 };
        return answer(args, &o, sizeof(o));
    }
    default:
        /* OPTIONS_0102 carries minimum sample counts the runtime checks, the
         * 106 features are validated against the feature level, and the
         * deprecated/video types have no answer at 0040: unsupported. */
        return E_INVALIDARG;
    }
}

HRESULT APIENTRY d3d12_umd_get_caps(D3D12DDI_HADAPTER hAdapter, const D3D12DDIARG_GETCAPS *pArgs) {
    const D3D12_UMD_Adapter *adapter = umd_adapter(hAdapter);
    HRESULT hr = pArgs->pData && pArgs->DataSize ? get_caps(adapter, pArgs) : E_INVALIDARG;
    INFO("%s: %d %s size=%u info=%p hr=0x%08lx", __FUNCTION__, pArgs->Type, caps_name(pArgs->Type),
         pArgs->DataSize, pArgs->pInfo, hr);
    return hr;
}
