#pragma once

#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winddk_compat.h>
/* d3d10umddi.h defines the D3D10 hardware constants as non-static C objects;
 * nothing here uses them. */
#define _D3D10_CONSTANTS
#define _D3D10_1_CONSTANTS
#include <d3d10umddi.h>
#include <d3dumddi.h>
#include <d3d12umddi.h>

#include <dxgi.h>
/* The DDI headers already provide the SDK DXGI types. */
#define __vkd3d_dxgiformat_h__
#define __vkd3d_dxgitype_h__
#include <vkd3d_d3d12.h>

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#define VKD3D_NO_WIN32_TYPES
#include <vkd3d.h>

#include <vulkan_d3dddi.h>
#include <virtio_wddm_uapi.h>

#include "vk_bootstrap.h"

#ifndef ERROR_GRAPHICS_DRIVER_MISMATCH
#define ERROR_GRAPHICS_DRIVER_MISMATCH _HRESULT_TYPEDEF_(0xC0262009L)
#endif

/* Logging: off unless VIRTIO_WDDM_LOG_FILE names a file ("1" = C:\dx12um_log.txt),
 * the same switch as dx11um. Stub hits also go to OutputDebugString. */
void virtio_wddm_log(const char *file, int line, const char *label, const char *format, ...) __attribute__((format(printf, 4, 5)));
bool d3d12_umd_log_enabled(void);
#undef ERROR
#define ERROR(...) virtio_wddm_log(__FILE__, __LINE__, "ERROR", __VA_ARGS__)
#define WARN(...) virtio_wddm_log(__FILE__, __LINE__, "WARN", __VA_ARGS__)
#define INFO(...) virtio_wddm_log(__FILE__, __LINE__, "INFO", __VA_ARGS__)
#define TRACE_CALL() do { if (d3d12_umd_log_enabled()) INFO("%s", __FUNCTION__); } while (0)
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(*(x)))

/* Registry switches, from the adapter's software key (INF HKR). */
enum {
    /* D3D12Enable: 0 = refuse OpenAdapter12, 1 = every process but dwm.exe,
     * 2 = dwm.exe too. Missing registry values default to applications on. */
    D3D12_UMD_ENABLE_OFF = 0,
    D3D12_UMD_ENABLE_APPS = 1,
    D3D12_UMD_ENABLE_ALL = 2,
};

typedef struct {
    D3D12DDI_HRTADAPTER hRTAdapter;
    D3DDDI_ADAPTERCALLBACKS callbacks;
    VIRTIO_WDDM_AdapterCaps caps;
    /* D3D12FeatureLevelOverride: 0 or a D3D_FEATURE_LEVEL (0xc000 = 12_0). */
    DWORD feature_level_override;
    /* The negotiated DDI version, from the first CalcPrivateDeviceSize. */
    UINT interface_version;
    UINT version;
    /* hRTTable of each COMMAND_LIST_3D fill, handed to SetCommandListDDITableCb. */
    D3D12DDI_HRTTABLE cl_tables[2];
} D3D12_UMD_Adapter;

typedef struct {
    D3D12_UMD_Adapter *adapter;
    D3D12DDI_HRTDEVICE hRTDevice;
    D3DDDI_DEVICECALLBACKS kt;
    const D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 *um;
    const D3D12DDI_CORELAYER_DEVICECALLBACKS_0022 *um22;
    D3D12DDI_CREATE_DEVICE_FLAGS flags;

    VIRTIO_WDDM_VkBootstrap vk_bootstrap;
    VkD3DDDICallbacks callbacks;
    VkD3DDDIRuntimeAllocator runtime_allocator;
    struct {
        D3DKMT_HANDLE queue, sync_object;
    } paging;
    VkInstance vk_instance;
    VkPhysicalDevice vk_physical;
    VkDevice vk_device;
    PFN_vkGetDeviceProcAddr vk_gdpa;

    ID3D12Device *d3d12;
    ID3D12Device2 *d3d12_2; /* CreatePipelineState: pipeline streams */
    ID3D12Device4 *d3d12_4; /* CreateCommandList1: lists that start closed */
    UINT descriptor_size[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES];
    SRWLOCK mapped_texture_lock;
    struct D3D12_UMD_Heap *mapped_textures;
} D3D12_UMD_Device;

static inline D3D12_UMD_Device *umd_device(D3D12DDI_HDEVICE h) { return h.pDrvPrivate; }
static inline D3D12_UMD_Adapter *umd_adapter(D3D12DDI_HADAPTER h) { return h.pDrvPrivate; }

/* A failed VOID DDI reports here; the runtime removes the device. */
void d3d12_umd_set_error(D3D12_UMD_Device *device, HRESULT hr);
HRESULT d3d12_umd_flush_mapped_textures(D3D12_UMD_Device *device);

/* ---------------------------------------------------------------------------
 * Driver objects. Each lives in the private memory the runtime sizes with the
 * matching CalcPrivate*Size, and wraps the vkd3d object the DDI forwards to.
 */

typedef struct D3D12_UMD_Heap D3D12_UMD_Heap;

typedef struct D3D12_UMD_Resource {
    ID3D12Resource *res;
    D3D12_RESOURCE_DESC desc;
    /* The heap a buffer or placed resource lives in, and where: placed
     * resources name a buffer of that heap as their parent. NULL for
     * committed textures. */
    D3D12_UMD_Heap *heap;
    UINT64 heap_offset;
    /* Primary images belong to this resource, not to vkd3d's borrowed wrapper. */
    VkImage primary_image;
    VkDeviceMemory primary_memory;
    D3DKMT_HANDLE hKMAllocation;
    D3DKMT_HANDLE hKMResource;
    D3D12DDI_HRTRESOURCE hRTResource;
    struct D3D12_UMD_Resource *present_shadow;
} D3D12_UMD_Resource;

struct D3D12_UMD_Heap {
    ID3D12Heap *heap;
    D3D12_HEAP_DESC desc;
    /* A buffer over the whole heap from offset 0 (the runtime creates one with
     * every heap), used for MapHeap. */
    ID3D12Resource *span;
    /* Committed textures have no ID3D12Heap. */
    ID3D12Resource *committed;
    /* The runtime resource handle the committed texture belongs to, and
     * whether its sysmem wrap was attempted: done lazily from MapHeap, when
     * the runtime resource is fully built and its handle is live. */
    void *hRTResource;
    bool sysmem_wrapped;
    D3DKMT_HANDLE sysmem_allocation;
    bool cpu_mapped;
    struct D3D12_UMD_Heap *mapped_next;
};

typedef struct {
    ID3D12DescriptorHeap *heap;
    D3D12DDI_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12DDI_GPU_DESCRIPTOR_HANDLE gpu;
} D3D12_UMD_DescriptorHeap;

typedef struct {
    ID3D12QueryHeap *heap;
} D3D12_UMD_QueryHeap;

typedef struct {
    ID3D12CommandSignature *sig;
} D3D12_UMD_CommandSignature;

typedef struct {
    ID3D12RootSignature *rs;
} D3D12_UMD_RootSignature;

/* The runtime hands each create the DXIL program part ([program token]
 * [length in DWORDs][DXIL ...]) and the stage's IO signatures by register;
 * vkd3d wants a DXBC container, so one is built at create time. */
typedef struct {
    UINT program_type; /* D3D10_SB_TOKENIZED_PROGRAM_TYPE: 0 PS, 1 VS, 2 GS, 3 HS, 4 DS, 5 CS */
    void *dxbc;
    SIZE_T dxbc_size;
    /* Stream output, when created through CreateGeometryShaderWithStreamOutput.
     * The shader may be the last pre-rasterization stage rather than a GS. */
    D3D12_SO_DECLARATION_ENTRY *so_entries;
    UINT so_count;
    UINT so_strides[D3D12_SO_BUFFER_SLOT_COUNT];
    UINT so_num_strides;
    UINT so_rasterized_stream;
} D3D12_UMD_Shader;

typedef struct {
    ID3D12PipelineState *pso;
} D3D12_UMD_PipelineState;

typedef struct {
    D3D12DDI_BLEND_DESC_0010 desc;
} D3D12_UMD_BlendState;

typedef struct {
    D3D12DDI_DEPTH_STENCIL_DESC_0025 desc;
    UINT8 back_read_mask, back_write_mask;
} D3D12_UMD_DepthStencilState;

typedef struct {
    D3D12DDI_RASTERIZER_DESC_0010 desc;
} D3D12_UMD_RasterizerState;

typedef struct {
    UINT num_elements;
    D3D12DDIARG_INPUT_ELEMENT_DESC *elements;
} D3D12_UMD_ElementLayout;

/* A DDI pool has no list type but a vkd3d allocator does, so a pool holds one
 * allocator per type, created on first use. */
enum { D3D12_UMD_LIST_TYPES = 4 }; /* DIRECT, BUNDLE, COMPUTE, COPY */

typedef struct {
    D3D12_UMD_Device *device;
    ID3D12CommandAllocator *allocators[D3D12_UMD_LIST_TYPES];
} D3D12_UMD_CommandPool;

typedef struct {
    D3D12DDI_COMMAND_QUEUE_FLAGS queue_flags;
    D3D12_UMD_CommandPool *pool;
} D3D12_UMD_CommandRecorder;

typedef struct {
    D3D12_UMD_Device *device;
    D3D12DDI_HRTCOMMANDLIST hRT;
    ID3D12GraphicsCommandList2 *cl;
    D3D12_COMMAND_LIST_TYPE type;
    bool synchronous_present;
} D3D12_UMD_CommandList;

typedef struct {
    D3D12DDI_FENCE fence;
} D3D12_UMD_Fence;

static inline D3D12_UMD_Resource *umd_resource(D3D12DDI_HRESOURCE h) { return h.pDrvPrivate; }
static inline ID3D12Resource *umd_res(D3D12DDI_HRESOURCE h) {
    D3D12_UMD_Resource *r = h.pDrvPrivate;
    return r ? r->res : NULL;
}
static inline D3D12_UMD_CommandList *umd_cl(D3D12DDI_HCOMMANDLIST h) { return h.pDrvPrivate; }
static inline ID3D12RootSignature *umd_root_signature(D3D12DDI_HROOTSIGNATURE h) {
    D3D12_UMD_RootSignature *rs = h.pDrvPrivate;
    return rs ? rs->rs : NULL;
}

/* d3d12_adapter.c */
bool d3d12_umd_is_dwm(void);

/* d3d12_kmt_trace.c */
void d3d12_umd_kmt_trace_init(void);

/* d3d12_caps.c */
HRESULT APIENTRY d3d12_umd_get_caps(D3D12DDI_HADAPTER hAdapter, const D3D12DDIARG_GETCAPS *pArgs);
D3D12DDI_3DPIPELINELEVEL d3d12_umd_pipeline_level(const D3D12_UMD_Adapter *adapter);

/* d3d12_device.c */
SIZE_T APIENTRY d3d12_umd_calc_private_device_size(D3D12DDI_HADAPTER hAdapter, const D3D12DDIARG_CALCPRIVATEDEVICESIZE *pArgs);
HRESULT APIENTRY d3d12_umd_create_device(D3D12DDI_HADAPTER hAdapter, const D3D12DDIARG_CREATEDEVICE_0003 *pArgs);
void APIENTRY d3d12_umd_destroy_device(D3D12DDI_HDEVICE hDevice);

/* d3d12_objects.c: table assembly */
void d3d12_umd_fill_core(D3D12DDI_DEVICE_FUNCS_CORE_0040 *t);
void d3d12_umd_fill_command_list(D3D12DDI_COMMAND_LIST_FUNCS_3D_0040 *t);
void d3d12_umd_fill_command_queue(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *t);
void d3d12_umd_fill_extended_features(D3D12DDI_EXTENDED_FEATURES_FUNCS_0021 *t);

/* Installers: each overwrites the slots its file implements. */
void d3d12_umd_install_queue_core(D3D12DDI_DEVICE_FUNCS_CORE_0040 *t);
void d3d12_umd_install_queue(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *t);
void d3d12_umd_install_cmdlist_core(D3D12DDI_DEVICE_FUNCS_CORE_0040 *t);
void d3d12_umd_install_cmdlist(D3D12DDI_COMMAND_LIST_FUNCS_3D_0040 *t);
void d3d12_umd_install_resource_core(D3D12DDI_DEVICE_FUNCS_CORE_0040 *t);
void d3d12_umd_install_descriptor_core(D3D12DDI_DEVICE_FUNCS_CORE_0040 *t);
void d3d12_umd_install_pipeline_core(D3D12DDI_DEVICE_FUNCS_CORE_0040 *t);

/* d3d12_resource.c */
void d3d12_umd_resource_desc(const D3D12DDIARG_CREATERESOURCE_0003 *args, D3D12_RESOURCE_DESC *desc);
bool d3d12_umd_shareable_texture(const D3D12_RESOURCE_DESC *desc);
HRESULT d3d12_umd_create_linear_texture(D3D12_UMD_Device *device, D3D12_UMD_Resource *resource,
                                      D3D12DDI_HRTRESOURCE hRTResource, bool primary);
HRESULT d3d12_umd_create_primary(D3D12_UMD_Device *device, D3D12_UMD_Resource *resource,
                                  D3D12DDI_HRTRESOURCE hRTResource);
void d3d12_umd_destroy_primary(D3D12_UMD_Device *device, D3D12_UMD_Resource *resource);
void d3d12_umd_init_runtime_allocator(D3D12_UMD_Device *device);
HRESULT d3d12_umd_init_present_device(D3D12_UMD_Device *device);
void d3d12_umd_release_present_device(D3D12_UMD_Device *device);
HANDLE d3d12_umd_present_context(D3D12DDI_HCOMMANDQUEUE hQueue);

/* ddi_stubs.c (generated by gen_ddi_stubs.py) */
void d3d12_umd_stub_hit(const char *table, unsigned slot, const char *name, volatile LONG *hits);
void d3d12_umd_stub_fill_core(D3D12DDI_DEVICE_FUNCS_CORE_0040 *t);
void d3d12_umd_stub_fill_cl(D3D12DDI_COMMAND_LIST_FUNCS_3D_0040 *t);
void d3d12_umd_stub_fill_queue(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *t);
void d3d12_umd_stub_fill_ext(D3D12DDI_EXTENDED_FEATURES_FUNCS_0021 *t);

void d3d12_umd_fill_modern_core(D3D12DDI_DEVICE_FUNCS_CORE_0096 *t);
void d3d12_umd_fill_modern_command_list(D3D12DDI_COMMAND_LIST_FUNCS_3D_0095 *t);
void d3d12_umd_fill_modern_extended_features(D3D12DDI_EXTENDED_FEATURES_FUNCS_0096 *t);
