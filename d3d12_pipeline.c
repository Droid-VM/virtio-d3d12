#include "d3d12_umd.h"

/* ---------------------------------------------------------------------------
 * Pipeline objects (dx12plan.md §2.3): the fixed-function sub-states keep
 * their DDI description until CreatePipelineState assembles them; shaders
 * become DXBC containers; root signatures, PSOs are vkd3d objects.
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
 * Shaders. The runtime passes the DXIL part of the application's container
 * (DxilProgramHeader: program token, size in DWORDs, 'DXIL', bitcode) and the
 * IO signatures resolved to registers. dxil-spirv reads everything it compiles
 * from the DXIL module; vkd3d only uses the ISG1/OSG1/PSG1 parts to match
 * stages, input layouts and stream output, and it matches them by name. The
 * semantics are gone at the DDI, so every element is named after where it
 * lives, and the same rule on both sides of each link keeps them matched.
 */

#define TAG(a, b, c, d) ((UINT)(a) | (UINT)(b) << 8 | (UINT)(c) << 16 | (UINT)(d) << 24)
#define TAG_DXBC TAG('D', 'X', 'B', 'C')
#define TAG_ISG1 TAG('I', 'S', 'G', '1')
#define TAG_OSG1 TAG('O', 'S', 'G', '1')
#define TAG_PSG1 TAG('P', 'S', 'G', '1')
#define TAG_DXIL TAG('D', 'X', 'I', 'L')

enum { PROGRAM_PS = 0, PROGRAM_VS = 1, PROGRAM_GS = 2, PROGRAM_HS = 3, PROGRAM_DS = 4, PROGRAM_CS = 5 };

#define REGISTER_NONE 0xffffffffu /* PS depth/coverage/stencil outputs; stream-output gaps */

/* D3D10_SB_NAME -> semantic name; the tessellation factors fold onto the
 * D3D_NAME the container carries. */
static const struct { const char *name; UINT d3d_name; } sysvals[] = {
    [D3D10_SB_NAME_POSITION] = { "SV_Position", 1 },
    [D3D10_SB_NAME_CLIP_DISTANCE] = { "SV_ClipDistance", 2 },
    [D3D10_SB_NAME_CULL_DISTANCE] = { "SV_CullDistance", 3 },
    [D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX] = { "SV_RenderTargetArrayIndex", 4 },
    [D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX] = { "SV_ViewportArrayIndex", 5 },
    [D3D10_SB_NAME_VERTEX_ID] = { "SV_VertexID", 6 },
    [D3D10_SB_NAME_PRIMITIVE_ID] = { "SV_PrimitiveID", 7 },
    [D3D10_SB_NAME_INSTANCE_ID] = { "SV_InstanceID", 8 },
    [D3D10_SB_NAME_IS_FRONT_FACE] = { "SV_IsFrontFace", 9 },
    [D3D10_SB_NAME_SAMPLE_INDEX] = { "SV_SampleIndex", 10 },
    [D3D11_SB_NAME_FINAL_QUAD_U_EQ_0_EDGE_TESSFACTOR] = { "SV_TessFactor", 11 },
    [D3D11_SB_NAME_FINAL_QUAD_V_EQ_0_EDGE_TESSFACTOR] = { "SV_TessFactor", 11 },
    [D3D11_SB_NAME_FINAL_QUAD_U_EQ_1_EDGE_TESSFACTOR] = { "SV_TessFactor", 11 },
    [D3D11_SB_NAME_FINAL_QUAD_V_EQ_1_EDGE_TESSFACTOR] = { "SV_TessFactor", 11 },
    [D3D11_SB_NAME_FINAL_QUAD_U_INSIDE_TESSFACTOR] = { "SV_InsideTessFactor", 12 },
    [D3D11_SB_NAME_FINAL_QUAD_V_INSIDE_TESSFACTOR] = { "SV_InsideTessFactor", 12 },
    [D3D11_SB_NAME_FINAL_TRI_U_EQ_0_EDGE_TESSFACTOR] = { "SV_TessFactor", 13 },
    [D3D11_SB_NAME_FINAL_TRI_V_EQ_0_EDGE_TESSFACTOR] = { "SV_TessFactor", 13 },
    [D3D11_SB_NAME_FINAL_TRI_W_EQ_0_EDGE_TESSFACTOR] = { "SV_TessFactor", 13 },
    [D3D11_SB_NAME_FINAL_TRI_INSIDE_TESSFACTOR] = { "SV_InsideTessFactor", 14 },
    [D3D11_SB_NAME_FINAL_LINE_DETAIL_TESSFACTOR] = { "SV_TessFactor", 15 },
    [D3D11_SB_NAME_FINAL_LINE_DENSITY_TESSFACTOR] = { "SV_TessFactor", 16 },
    /* D3D12_SB_NAME_BARYCENTRICS / SHADINGRATE / CULLPRIMITIVE, newer than
     * the tokenized-format header in use. */
    [23] = { "SV_Barycentrics", 23 },
    [24] = { "SV_ShadingRate", 24 },
    [25] = { "SV_CullPrimitive", 25 },
};

typedef struct {
    const D3D12DDIARG_SIGNATURE_ENTRY_0012 *entries;
    UINT count;
    UINT tag;
    /* VS inputs are named by register alone: the input layout only has that. */
    bool vs_input;
    bool ps_output;
} signature;

static const char *element_name(const signature *sig, const D3D12DDIARG_SIGNATURE_ENTRY_0012 *e) {
    if (e->Register == REGISTER_NONE)
        return "SV_Depth";
    if ((UINT)e->SystemValue < ARRAY_SIZE(sysvals) && sysvals[e->SystemValue].name)
        return sysvals[e->SystemValue].name;
    return sig->ps_output ? "SV_Target" : "ATTRIB";
}

static UINT element_index(const signature *sig, UINT i) {
    const D3D12DDIARG_SIGNATURE_ENTRY_0012 *e = &sig->entries[i];
    if (e->Register == REGISTER_NONE)
        return i;
    if (sig->vs_input || !e->Mask)
        return e->Register * 4;
    return e->Register * 4 + __builtin_ctz(e->Mask);
}

static UINT element_sysval(const D3D12DDIARG_SIGNATURE_ENTRY_0012 *e) {
    return (UINT)e->SystemValue < ARRAY_SIZE(sysvals) ? sysvals[e->SystemValue].d3d_name : 0;
}

static SIZE_T signature_size(const signature *sig) {
    SIZE_T size = 8 + sig->count * 8 * sizeof(UINT);
    for (UINT i = 0; i < sig->count; i++)
        size += strlen(element_name(sig, &sig->entries[i])) + 1;
    return (size + 3) & ~(SIZE_T)3;
}

/* ISG1/OSG1/PSG1 payload: count, 8, then per element stream, name offset,
 * semantic index, sysval, component type, register, mask | rw mask << 8,
 * min precision; names follow. */
static void write_signature(const signature *sig, BYTE *out) {
    UINT *words = (UINT *)out;
    words[0] = sig->count;
    words[1] = 8;
    char *names = (char *)out + 8 + sig->count * 8 * sizeof(UINT);
    for (UINT i = 0; i < sig->count; i++) {
        const D3D12DDIARG_SIGNATURE_ENTRY_0012 *e = &sig->entries[i];
        const char *name = element_name(sig, e);
        UINT *w = &words[2 + i * 8];
        w[0] = e->Stream;
        w[1] = (UINT)(names - (char *)out);
        w[2] = element_index(sig, i);
        w[3] = element_sysval(e);
        w[4] = e->RegisterComponentType;
        w[5] = e->Register;
        /* Inputs: components read; outputs: components never written (none). */
        w[6] = e->Mask | (sig->tag == TAG_ISG1 ? (UINT)e->Mask << 8 : 0);
        w[7] = e->MinPrecision;
        size_t len = strlen(name) + 1;
        memcpy(names, name, len);
        names += len;
    }
}

static void *build_dxbc(const UINT *code, const signature *sigs, UINT sig_count, SIZE_T *size_out) {
    SIZE_T code_size = (SIZE_T)code[1] * sizeof(UINT);
    UINT chunks = sig_count + 1;
    SIZE_T size = 32 + chunks * sizeof(UINT);
    for (UINT i = 0; i < sig_count; i++)
        size += 8 + signature_size(&sigs[i]);
    size += 8 + code_size;

    BYTE *dxbc = calloc(1, size);
    if (!dxbc)
        return NULL;
    UINT *header = (UINT *)dxbc;
    header[0] = TAG_DXBC;
    /* header[1..4]: checksum, which vkd3d ignores. */
    header[5] = 1;
    header[6] = (UINT)size;
    header[7] = chunks;

    SIZE_T offset = 32 + chunks * sizeof(UINT);
    for (UINT i = 0; i < chunks; i++) {
        bool dxil = i == sig_count;
        SIZE_T payload = dxil ? code_size : signature_size(&sigs[i]);
        header[8 + i] = (UINT)offset;
        UINT *chunk = (UINT *)(dxbc + offset);
        chunk[0] = dxil ? TAG_DXIL : sigs[i].tag;
        chunk[1] = (UINT)payload;
        if (dxil)
            memcpy(chunk + 2, code, code_size);
        else
            write_signature(&sigs[i], (BYTE *)(chunk + 2));
        offset += 8 + payload;
    }
    *size_out = size;
    return dxbc;
}

static void log_signature(const char *what, const D3D12DDIARG_SIGNATURE_ENTRY_0012 *entries, UINT count) {
    for (UINT i = 0; i < count; i++)
        INFO("  %s[%u]: sv %d reg %u mask 0x%x stream %u type %d prec %d", what, i, entries[i].SystemValue,
             entries[i].Register, entries[i].Mask, entries[i].Stream, entries[i].RegisterComponentType,
             entries[i].MinPrecision);
}

/* Stream output names the elements it captures by register and mask; turn
 * them into the names the output signature gave those registers. */
static bool build_stream_output(D3D12_UMD_Shader *shader, const signature *out,
                                const D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT_0026 *args) {
    shader->so_entries = calloc(args->NumEntries ? args->NumEntries : 1, sizeof(*shader->so_entries));
    if (!shader->so_entries)
        return false;
    for (UINT i = 0; i < args->NumEntries; i++) {
        const D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY *d = &args->pOutputStreamDecl[i];
        D3D12_SO_DECLARATION_ENTRY *e = &shader->so_entries[i];
        UINT first = d->RegisterMask ? __builtin_ctz(d->RegisterMask) : 0;
        *e = (D3D12_SO_DECLARATION_ENTRY) {
            .Stream = d->Stream,
            .ComponentCount = (BYTE)__builtin_popcount(d->RegisterMask),
            .OutputSlot = (BYTE)d->OutputSlot,
        };
        if (d->RegisterIndex == REGISTER_NONE)
            continue; /* a gap: no name */
        for (UINT j = 0; j < out->count; j++) {
            const D3D12DDIARG_SIGNATURE_ENTRY_0012 *s = &out->entries[j];
            if (s->Register == d->RegisterIndex && s->Stream == d->Stream && (s->Mask & (1u << first))) {
                e->SemanticName = element_name(out, s);
                e->SemanticIndex = element_index(out, j);
                e->StartComponent = (BYTE)(first - __builtin_ctz(s->Mask));
                break;
            }
        }
        if (!e->SemanticName)
            WARN("stream output entry %u: register %u mask 0x%x not in the output signature", i, d->RegisterIndex,
                 d->RegisterMask);
    }
    shader->so_count = args->NumEntries;
    shader->so_num_strides = args->NumStrides < ARRAY_SIZE(shader->so_strides) ? args->NumStrides : ARRAY_SIZE(shader->so_strides);
    memcpy(shader->so_strides, args->BufferStridesInBytes, shader->so_num_strides * sizeof(UINT));
    shader->so_rasterized_stream = args->RasterizedStream;
    return true;
}

static SIZE_T APIENTRY calc_shader_size(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_SHADER_0026 *args) {
    (void)hDevice;
    (void)args;
    return sizeof(D3D12_UMD_Shader);
}

static void create_shader(D3D12_UMD_Device *device, const D3D12DDIARG_CREATE_SHADER_0026 *args, bool tessellation,
                          const D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT_0026 *so, D3D12DDI_HSHADER handle) {
    D3D12_UMD_Shader *shader = handle.pDrvPrivate;
    memset(shader, 0, sizeof(*shader));
    const UINT *code = args->pShaderCode;
    /* With no geometry shader the runtime supplies an SO-only object.
     * Its output signature still describes the preceding VS/DS outputs. */
    if (!code && so) {
        const D3D12DDIARG_STAGE_IO_SIGNATURES *io = args->IOSignatures.Standard;
        signature output = { io ? io->pOutputSignature : NULL,
                             io ? io->NumOutputSignatureEntries : 0, TAG_OSG1 };
        shader->program_type = PROGRAM_VS;
        if (!build_stream_output(shader, &output, so))
            d3d12_umd_set_error(device, E_OUTOFMEMORY);
        if (d3d12_umd_log_enabled())
            INFO("stream output only %p: %u entries, %u signature entries", shader, so->NumEntries, output.count);
        return;
    }
    if (!code || code[1] < 6 || code[1] > (1u << 24) || code[2] != TAG_DXIL) {
        ERROR("%s: not a DXIL program part (%08x %08x %08x)", __FUNCTION__, code ? code[0] : 0, code ? code[1] : 0,
              code ? code[2] : 0);
        d3d12_umd_set_error(device, E_INVALIDARG);
        return;
    }
    shader->program_type = code[0] >> 16;

    signature sigs[3];
    UINT n = 0;
    const D3D12DDIARG_SIGNATURE_ENTRY_0012 *in = NULL, *out = NULL, *patch = NULL;
    UINT n_in = 0, n_out = 0, n_patch = 0;
    if (tessellation && args->IOSignatures.Tessellation) {
        const D3D12DDIARG_TESSELLATION_IO_SIGNATURES *s = args->IOSignatures.Tessellation;
        in = s->pInputSignature, n_in = s->NumInputSignatureEntries;
        out = s->pOutputSignature, n_out = s->NumOutputSignatureEntries;
        patch = s->pPatchConstantSignature, n_patch = s->NumPatchConstantSignatureEntries;
    } else if (!tessellation && args->IOSignatures.Standard) {
        const D3D12DDIARG_STAGE_IO_SIGNATURES *s = args->IOSignatures.Standard;
        in = s->pInputSignature, n_in = s->NumInputSignatureEntries;
        out = s->pOutputSignature, n_out = s->NumOutputSignatureEntries;
    }
    if (shader->program_type != PROGRAM_CS) {
        sigs[n++] = (signature) { in, n_in, TAG_ISG1, .vs_input = shader->program_type == PROGRAM_VS };
        sigs[n++] = (signature) { out, n_out, TAG_OSG1, .ps_output = shader->program_type == PROGRAM_PS };
        if (tessellation)
            sigs[n++] = (signature) { patch, n_patch, TAG_PSG1 };
    }

    shader->dxbc = build_dxbc(code, sigs, n, &shader->dxbc_size);
    if (!shader->dxbc || (so && !build_stream_output(shader, &sigs[1], so))) {
        free(shader->dxbc);
        memset(shader, 0, sizeof(*shader));
        d3d12_umd_set_error(device, E_OUTOFMEMORY);
        return;
    }
    if (d3d12_umd_log_enabled()) {
        INFO("shader %p: program %u, %u dwords, %u/%u/%u signature entries%s", shader, shader->program_type, code[1],
             n_in, n_out, n_patch, so ? ", stream output" : "");
        log_signature("in", in, n_in);
        log_signature("out", out, n_out);
        log_signature("patch", patch, n_patch);
    }
}

static void APIENTRY create_standard_shader(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_SHADER_0026 *args,
                                            D3D12DDI_HSHADER handle) {
    create_shader(umd_device(hDevice), args, false, NULL, handle);
}

static void APIENTRY create_tessellation_shader(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_SHADER_0026 *args,
                                                D3D12DDI_HSHADER handle) {
    create_shader(umd_device(hDevice), args, true, NULL, handle);
}

static SIZE_T APIENTRY calc_geometry_shader_with_stream_output_size(
        D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT_0026 *args) {
    return calc_shader_size(hDevice, &args->CreateShader);
}

/* Also how stream output without a geometry shader arrives (NULL code). */
static void APIENTRY create_geometry_shader_with_stream_output(
        D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT_0026 *args,
        D3D12DDI_HSHADER handle) {
    create_shader(umd_device(hDevice), &args->CreateShader, false, args, handle);
}

static void APIENTRY destroy_shader(D3D12DDI_HDEVICE hDevice, D3D12DDI_HSHADER handle) {
    (void)hDevice;
    D3D12_UMD_Shader *shader = handle.pDrvPrivate;
    free(shader->dxbc);
    free(shader->so_entries);
    memset(shader, 0, sizeof(*shader));
}

/* ---------------------------------------------------------------------------
 * Root signatures arrive parsed, always as version 1.1; the 1.1 DDI tree is
 * laid out exactly like the API's, so it serializes as is into the RTS0 blob
 * vkd3d's CreateRootSignature wants.
 */

static_assert(sizeof(D3D12DDI_ROOT_SIGNATURE_0013) == sizeof(D3D12_ROOT_SIGNATURE_DESC1));
static_assert(offsetof(D3D12DDI_ROOT_SIGNATURE_0013, Flags) == offsetof(D3D12_ROOT_SIGNATURE_DESC1, Flags));
static_assert(sizeof(D3D12DDI_ROOT_PARAMETER_0013) == sizeof(D3D12_ROOT_PARAMETER1));
static_assert(offsetof(D3D12DDI_ROOT_PARAMETER_0013, DescriptorTable) == offsetof(D3D12_ROOT_PARAMETER1, DescriptorTable));
static_assert(offsetof(D3D12DDI_ROOT_PARAMETER_0013, ShaderVisibility) == offsetof(D3D12_ROOT_PARAMETER1, ShaderVisibility));
static_assert(sizeof(D3D12DDI_DESCRIPTOR_RANGE_0013) == sizeof(D3D12_DESCRIPTOR_RANGE1));
static_assert(offsetof(D3D12DDI_DESCRIPTOR_RANGE_0013, Flags) == offsetof(D3D12_DESCRIPTOR_RANGE1, Flags));
static_assert(sizeof(D3D12DDI_ROOT_DESCRIPTOR_0013) == sizeof(D3D12_ROOT_DESCRIPTOR1));
static_assert(sizeof(D3D12DDI_ROOT_CONSTANTS) == sizeof(D3D12_ROOT_CONSTANTS));
static_assert(sizeof(D3D12DDI_STATIC_SAMPLER) == sizeof(D3D12_STATIC_SAMPLER_DESC));
static_assert(offsetof(D3D12DDI_STATIC_SAMPLER, ShaderVisibility) == offsetof(D3D12_STATIC_SAMPLER_DESC, ShaderVisibility));

static SIZE_T APIENTRY calc_root_signature_size(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013 *args) {
    (void)hDevice;
    (void)args;
    return sizeof(D3D12_UMD_RootSignature);
}

static HRESULT APIENTRY create_root_signature(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013 *args,
                                              D3D12DDI_HROOTSIGNATURE handle) {
    D3D12_UMD_Device *device = umd_device(hDevice);
    D3D12_UMD_RootSignature *rs = handle.pDrvPrivate;
    rs->rs = NULL;
    if (args->Version != D3D12DDI_ROOT_SIGNATURE_VERSION_1_1 || !args->pRootSignature_1_1) {
        ERROR("%s: version %d", __FUNCTION__, args->Version);
        return E_INVALIDARG;
    }

    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {
        .Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
        .Desc_1_1 = *(const D3D12_ROOT_SIGNATURE_DESC1 *)args->pRootSignature_1_1,
    };
    ID3DBlob *blob = NULL, *error = NULL;
    HRESULT hr = vkd3d_serialize_versioned_root_signature(&desc, &blob, &error);
    if (SUCCEEDED(hr)) {
        hr = ID3D12Device_CreateRootSignature(device->d3d12, 0, ID3D10Blob_GetBufferPointer(blob),
                                              ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs->rs);
        ID3D10Blob_Release(blob);
    } else if (error) {
        ERROR("%s: %s", __FUNCTION__, (const char *)ID3D10Blob_GetBufferPointer(error));
    }
    if (error)
        ID3D10Blob_Release(error);
    if (FAILED(hr) || d3d12_umd_log_enabled())
        INFO("root signature %p: %u parameters, %u static samplers, flags 0x%x: 0x%08lx", rs, desc.Desc_1_1.NumParameters,
             desc.Desc_1_1.NumStaticSamplers, desc.Desc_1_1.Flags, hr);
    return hr;
}

static void APIENTRY destroy_root_signature(D3D12DDI_HDEVICE hDevice, D3D12DDI_HROOTSIGNATURE handle) {
    (void)hDevice;
    D3D12_UMD_RootSignature *rs = handle.pDrvPrivate;
    if (rs->rs)
        ID3D12RootSignature_Release(rs->rs);
    rs->rs = NULL;
}

/* ---------------------------------------------------------------------------
 * Pipeline states. Graphics goes through a pipeline stream: it is the only way
 * to hand vkd3d the depth-bounds enable.
 */

/* The runtime owns application names and supplies stable numeric indices.
 * Retain initial data: the underlying library may reference its blob until
 * destruction, while the DDI input only lives through this callback. */
typedef struct {
    ID3D12PipelineLibrary1 *library;
    void *initial_data;
} pipeline_library;

static void pipeline_name(UINT index, WCHAR name[9]) {
    static const WCHAR digits[] = L"0123456789abcdef";
    for (UINT i = 0; i < 8; i++)
        name[i] = digits[(index >> (28 - 4 * i)) & 15];
    name[8] = 0;
}

static SIZE_T APIENTRY calc_pipeline_library_size(D3D12DDI_HDEVICE device,
        const D3D12DDIARG_CREATE_PIPELINE_LIBRARY_0010 *args) {
    (void)device;
    (void)args;
    return sizeof(pipeline_library);
}

static HRESULT APIENTRY create_pipeline_library(D3D12DDI_HDEVICE device,
        const D3D12DDIARG_CREATE_PIPELINE_LIBRARY_0010 *args, D3D12DDI_HPIPELINELIBRARY handle) {
    pipeline_library *lib = handle.pDrvPrivate;
    memset(lib, 0, sizeof(*lib));
    if (args->InitialDataSize) {
        if (!args->pInitialData)
            return E_INVALIDARG;
        lib->initial_data = malloc(args->InitialDataSize);
        if (!lib->initial_data)
            return E_OUTOFMEMORY;
        memcpy(lib->initial_data, args->pInitialData, args->InitialDataSize);
    }
    HRESULT hr = ID3D12Device2_CreatePipelineLibrary(umd_device(device)->d3d12_2,
            lib->initial_data, args->InitialDataSize, &IID_ID3D12PipelineLibrary1, (void **)&lib->library);
    if (FAILED(hr)) {
        free(lib->initial_data);
        lib->initial_data = NULL;
    }
    return hr;
}

static void APIENTRY destroy_pipeline_library(D3D12DDI_HDEVICE device, D3D12DDI_HPIPELINELIBRARY handle) {
    (void)device;
    pipeline_library *lib = handle.pDrvPrivate;
    if (lib->library)
        ID3D12PipelineLibrary1_Release(lib->library);
    free(lib->initial_data);
    memset(lib, 0, sizeof(*lib));
}

static HRESULT APIENTRY add_pipeline_to_library(D3D12DDI_HDEVICE device, D3D12DDI_HPIPELINELIBRARY handle,
        D3D12DDI_HPIPELINESTATE state, UINT index) {
    (void)device;
    pipeline_library *lib = handle.pDrvPrivate;
    D3D12_UMD_PipelineState *pso = state.pDrvPrivate;
    WCHAR name[9];
    pipeline_name(index, name);
    return ID3D12PipelineLibrary1_StorePipeline(lib->library, name, pso->pso);
}

static SIZE_T APIENTRY calc_serialized_library_size(D3D12DDI_HDEVICE device, D3D12DDI_HPIPELINELIBRARY handle) {
    (void)device;
    pipeline_library *lib = handle.pDrvPrivate;
    return ID3D12PipelineLibrary1_GetSerializedSize(lib->library);
}

static HRESULT APIENTRY serialize_library(D3D12DDI_HDEVICE device, D3D12DDI_HPIPELINELIBRARY handle, void *data) {
    (void)device;
    pipeline_library *lib = handle.pDrvPrivate;
    return ID3D12PipelineLibrary1_Serialize(lib->library, data,
            ID3D12PipelineLibrary1_GetSerializedSize(lib->library));
}

#define SUBOBJECT(T) struct { alignas(void *) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type; T data; }

typedef struct {
    SUBOBJECT(ID3D12RootSignature *) root_signature;
    SUBOBJECT(D3D12_SHADER_BYTECODE) vs, ps, ds, hs, gs;
    SUBOBJECT(D3D12_STREAM_OUTPUT_DESC) stream_output;
    SUBOBJECT(D3D12_BLEND_DESC) blend;
    SUBOBJECT(UINT) sample_mask;
    SUBOBJECT(D3D12_RASTERIZER_DESC) rasterizer;
    SUBOBJECT(D3D12_DEPTH_STENCIL_DESC1) depth_stencil;
    SUBOBJECT(D3D12_INPUT_LAYOUT_DESC) input_layout;
    SUBOBJECT(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE) strip_cut;
    SUBOBJECT(D3D12_PRIMITIVE_TOPOLOGY_TYPE) topology;
    SUBOBJECT(struct D3D12_RT_FORMAT_ARRAY) rtv_formats; /* no typedef in mingw */
    SUBOBJECT(DXGI_FORMAT) dsv_format;
    SUBOBJECT(DXGI_SAMPLE_DESC) sample_desc;
} graphics_stream;

static D3D12_SHADER_BYTECODE bytecode(D3D12DDI_HSHADER handle) {
    const D3D12_UMD_Shader *shader = handle.pDrvPrivate;
    return shader && shader->dxbc ? (D3D12_SHADER_BYTECODE) { shader->dxbc, shader->dxbc_size }
                                   : (D3D12_SHADER_BYTECODE) { 0 };
}

static D3D12_BLEND_DESC blend_desc(D3D12DDI_HBLENDSTATE handle) {
    static_assert(sizeof(D3D12DDI_RENDER_TARGET_BLEND_DESC) == sizeof(D3D12_RENDER_TARGET_BLEND_DESC));
    const D3D12_UMD_BlendState *state = handle.pDrvPrivate;
    D3D12_BLEND_DESC d = { .RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL };
    if (!state)
        return d;
    d.AlphaToCoverageEnable = state->desc.AlphaToCoverageEnable;
    d.IndependentBlendEnable = state->desc.IndependentBlendEnable;
    memcpy(d.RenderTarget, state->desc.RenderTarget, sizeof(d.RenderTarget));
    return d;
}

static D3D12_RASTERIZER_DESC rasterizer_desc(D3D12DDI_HRASTERIZERSTATE handle) {
    const D3D12_UMD_RasterizerState *state = handle.pDrvPrivate;
    if (!state)
        return (D3D12_RASTERIZER_DESC) { .FillMode = D3D12_FILL_MODE_SOLID, .CullMode = D3D12_CULL_MODE_BACK,
                                         .DepthClipEnable = TRUE };
    const D3D12DDI_RASTERIZER_DESC_0010 *s = &state->desc;
    /* ScissorEnable has no API field: D3D12 always scissors. */
    return (D3D12_RASTERIZER_DESC) {
        .FillMode = (D3D12_FILL_MODE)s->FillMode,
        .CullMode = (D3D12_CULL_MODE)s->CullMode,
        .FrontCounterClockwise = s->FrontCounterClockwise,
        .DepthBias = s->DepthBias,
        .DepthBiasClamp = s->DepthBiasClamp,
        .SlopeScaledDepthBias = s->SlopeScaledDepthBias,
        .DepthClipEnable = s->DepthClipEnable,
        .MultisampleEnable = s->MultisampleEnable,
        .AntialiasedLineEnable = s->AntialiasedLineEnable,
        .ForcedSampleCount = s->ForcedSampleCount,
        .ConservativeRaster = (D3D12_CONSERVATIVE_RASTERIZATION_MODE)s->ConservativeRasterizationMode,
    };
}

static D3D12_DEPTH_STENCILOP_DESC stencil_op(const D3D12DDI_DEPTH_STENCILOP_DESC *op, BOOL enable) {
    static const D3D12_DEPTH_STENCILOP_DESC keep = {
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS,
    };
    if (!enable)
        return keep;
    return (D3D12_DEPTH_STENCILOP_DESC) {
        (D3D12_STENCIL_OP)op->StencilFailOp, (D3D12_STENCIL_OP)op->StencilDepthFailOp,
        (D3D12_STENCIL_OP)op->StencilPassOp, (D3D12_COMPARISON_FUNC)op->StencilFunc,
    };
}

static D3D12_DEPTH_STENCIL_DESC1 depth_stencil_desc(D3D12DDI_HDEPTHSTENCILSTATE handle) {
    const D3D12_UMD_DepthStencilState *state = handle.pDrvPrivate;
    if (!state)
        return (D3D12_DEPTH_STENCIL_DESC1) {
            .DepthEnable = TRUE, .DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL, .DepthFunc = D3D12_COMPARISON_FUNC_LESS,
            .FrontFace = stencil_op(NULL, FALSE), .BackFace = stencil_op(NULL, FALSE),
        };
    const D3D12DDI_DEPTH_STENCIL_DESC_0025 *s = &state->desc;
    /* The DDI can switch the stencil test per face; the API cannot, so a
     * disabled face keeps its stencil. */
    return (D3D12_DEPTH_STENCIL_DESC1) {
        .DepthEnable = s->DepthEnable,
        .DepthWriteMask = (D3D12_DEPTH_WRITE_MASK)s->DepthWriteMask,
        .DepthFunc = (D3D12_COMPARISON_FUNC)s->DepthFunc,
        .StencilEnable = s->StencilEnable,
        .StencilReadMask = s->StencilReadMask,
        .StencilWriteMask = s->StencilWriteMask,
        .FrontFace = stencil_op(&s->FrontFace, s->FrontEnable),
        .BackFace = stencil_op(&s->BackFace, s->BackEnable),
        .DepthBoundsTestEnable = s->DepthBoundsTestEnable,
    };
}

/* Element names follow the VS input signature's: by register. */
static D3D12_INPUT_ELEMENT_DESC *input_layout(D3D12DDI_HELEMENTLAYOUT handle, UINT *count) {
    const D3D12_UMD_ElementLayout *layout = handle.pDrvPrivate;
    *count = 0;
    if (!layout || !layout->num_elements)
        return NULL;
    D3D12_INPUT_ELEMENT_DESC *elements = calloc(layout->num_elements, sizeof(*elements));
    if (!elements)
        return NULL;
    for (UINT i = 0; i < layout->num_elements; i++) {
        const D3D12DDIARG_INPUT_ELEMENT_DESC *e = &layout->elements[i];
        elements[i] = (D3D12_INPUT_ELEMENT_DESC) {
            .SemanticName = "ATTRIB",
            .SemanticIndex = e->InputRegister * 4,
            .Format = e->Format,
            .InputSlot = e->InputSlot,
            .AlignedByteOffset = e->AlignedByteOffset,
            .InputSlotClass = (D3D12_INPUT_CLASSIFICATION)e->InputSlotClass,
            .InstanceDataStepRate = e->InstanceDataStepRate,
        };
    }
    *count = layout->num_elements;
    return elements;
}

static HRESULT create_graphics_pipeline(D3D12_UMD_Device *device, const D3D12DDIARG_CREATE_PIPELINE_STATE_0033 *args,
                                        ID3D12PipelineState **pso) {
    graphics_stream s;
    memset(&s, 0, sizeof(s));
    s.root_signature.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
    s.root_signature.data = umd_root_signature(args->hRootSignature);
    s.vs.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS;
    s.vs.data = bytecode(args->hVertexShader);
    s.ps.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS;
    s.ps.data = bytecode(args->hPixelShader);
    s.ds.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS;
    s.ds.data = bytecode(args->hDomainShader);
    s.hs.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS;
    s.hs.data = bytecode(args->hHullShader);
    s.gs.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS;
    s.stream_output.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT;
    const D3D12_UMD_Shader *gs = args->hGeometryShader.pDrvPrivate;
    if (gs && gs->program_type == PROGRAM_GS)
        s.gs.data = bytecode(args->hGeometryShader);
    if (gs && gs->so_entries)
        s.stream_output.data = (D3D12_STREAM_OUTPUT_DESC) {
            gs->so_entries, gs->so_count, gs->so_strides, gs->so_num_strides, gs->so_rasterized_stream,
        };
    s.blend.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND;
    s.blend.data = blend_desc(args->hBlendState);
    s.sample_mask.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK;
    s.sample_mask.data = args->SampleMask;
    s.rasterizer.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER;
    s.rasterizer.data = rasterizer_desc(args->hRasterizerState);
    s.depth_stencil.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1;
    s.depth_stencil.data = depth_stencil_desc(args->hDepthStencilState);
    s.input_layout.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT;
    D3D12_INPUT_ELEMENT_DESC *elements = input_layout(args->hElementLayout, &s.input_layout.data.NumElements);
    s.input_layout.data.pInputElementDescs = elements;
    s.strip_cut.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE;
    s.strip_cut.data = (D3D12_INDEX_BUFFER_STRIP_CUT_VALUE)args->IBStripCutValue;
    s.topology.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY;
    s.topology.data = (D3D12_PRIMITIVE_TOPOLOGY_TYPE)args->PrimitiveTopologyType;
    s.rtv_formats.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
    s.rtv_formats.data.NumRenderTargets = args->NumRenderTargets;
    memcpy(s.rtv_formats.data.RTFormats, args->RTVFormats, sizeof(s.rtv_formats.data.RTFormats));
    s.dsv_format.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT;
    s.dsv_format.data = args->DSVFormat;
    s.sample_desc.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC;
    s.sample_desc.data = args->SampleDesc;

    D3D12_PIPELINE_STATE_STREAM_DESC desc = { sizeof(s), &s };
    pipeline_library *lib = args->LibraryReference.hLibrary.pDrvPrivate;
    HRESULT hr;
    if (lib) {
        WCHAR name[9];
        pipeline_name(args->LibraryReference.PipelineIndex, name);
        hr = ID3D12PipelineLibrary1_LoadPipeline(lib->library, name, &desc, &IID_ID3D12PipelineState, (void **)pso);
    } else {
        hr = ID3D12Device2_CreatePipelineState(device->d3d12_2, &desc, &IID_ID3D12PipelineState, (void **)pso);
    }
    free(elements);
    return hr;
}

static SIZE_T APIENTRY calc_pipeline_state_size(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_PIPELINE_STATE_0033 *args) {
    (void)hDevice;
    (void)args;
    return sizeof(D3D12_UMD_PipelineState);
}

static HRESULT APIENTRY create_pipeline_state(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_PIPELINE_STATE_0033 *args,
                                              D3D12DDI_HPIPELINESTATE handle, D3D12DDI_HRTPIPELINESTATE rt) {
    (void)rt;
    D3D12_UMD_Device *device = umd_device(hDevice);
    D3D12_UMD_PipelineState *pso = handle.pDrvPrivate;
    pso->pso = NULL;

    HRESULT hr;
    bool compute = args->hComputeShader.pDrvPrivate != NULL;
    if (compute) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {
            .pRootSignature = umd_root_signature(args->hRootSignature),
            .CS = bytecode(args->hComputeShader),
        };
        pipeline_library *lib = args->LibraryReference.hLibrary.pDrvPrivate;
        if (lib) {
            WCHAR name[9];
            pipeline_name(args->LibraryReference.PipelineIndex, name);
            hr = ID3D12PipelineLibrary1_LoadComputePipeline(lib->library, name, &desc,
                    &IID_ID3D12PipelineState, (void **)&pso->pso);
        } else {
            hr = ID3D12Device_CreateComputePipelineState(device->d3d12, &desc,
                    &IID_ID3D12PipelineState, (void **)&pso->pso);
        }
    } else {
        hr = create_graphics_pipeline(device, args, &pso->pso);
    }
    if (FAILED(hr) || d3d12_umd_log_enabled())
        INFO("pipeline state %p: %s, %u render targets, dsv %u, samples %u: 0x%08lx", pso, compute ? "compute" : "graphics",
             args->NumRenderTargets, args->DSVFormat, args->SampleDesc.Count, hr);
    return hr;
}

static void APIENTRY destroy_pipeline_state(D3D12DDI_HDEVICE hDevice, D3D12DDI_HPIPELINESTATE handle) {
    (void)hDevice;
    D3D12_UMD_PipelineState *pso = handle.pDrvPrivate;
    if (pso->pso)
        ID3D12PipelineState_Release(pso->pso);
    pso->pso = NULL;
}

void d3d12_umd_install_pipeline_core(D3D12DDI_DEVICE_FUNCS_CORE_0040 *t) {
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
    t->pfnCreateVertexShader = create_standard_shader;
    t->pfnCreatePixelShader = create_standard_shader;
    t->pfnCreateGeometryShader = create_standard_shader;
    t->pfnCreateComputeShader = create_standard_shader;
    t->pfnCalcPrivateGeometryShaderWithStreamOutput = calc_geometry_shader_with_stream_output_size;
    t->pfnCreateGeometryShaderWithStreamOutput = create_geometry_shader_with_stream_output;
    t->pfnCalcPrivateTessellationShaderSize = calc_shader_size;
    t->pfnCreateHullShader = create_tessellation_shader;
    t->pfnCreateDomainShader = create_tessellation_shader;
    t->pfnDestroyShader = destroy_shader;

    t->pfnCalcPrivateRootSignatureSize = calc_root_signature_size;
    t->pfnCreateRootSignature = create_root_signature;
    t->pfnDestroyRootSignature = destroy_root_signature;

    t->pfnCalcPrivatePipelineStateSize = calc_pipeline_state_size;
    t->pfnCreatePipelineState = create_pipeline_state;
    t->pfnDestroyPipelineState = destroy_pipeline_state;
    t->pfnCalcPrivatePipelineLibrarySize = calc_pipeline_library_size;
    t->pfnCreatePipelineLibrary = create_pipeline_library;
    t->pfnDestroyPipelineLibrary = destroy_pipeline_library;
    t->pfnAddPipelineStateToLibrary = add_pipeline_to_library;
    t->pfnCalcSerializedLibrarySize = calc_serialized_library_size;
    t->pfnSerializeLibrary = serialize_library;
}
