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

#include <d3d12.h>
#include <dxgi.h>

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
    /* D3D12Enable: 0 = refuse OpenAdapter12 (the runtime then behaves exactly
     * as with no D3D12 UMD), 1 = every process but dwm.exe, 2 = dwm.exe too. */
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
    D3D12DDI_CREATE_DEVICE_FLAGS flags;

    VIRTIO_WDDM_VkBootstrap vk_bootstrap;
    VkD3DDDICallbacks callbacks;

    ID3D12Device *d3d12;
    UINT descriptor_size[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES];
} D3D12_UMD_Device;

static inline D3D12_UMD_Device *umd_device(D3D12DDI_HDEVICE h) { return h.pDrvPrivate; }
static inline D3D12_UMD_Adapter *umd_adapter(D3D12DDI_HADAPTER h) { return h.pDrvPrivate; }

/* Objects the runtime creates before the command path exists (its own
 * internal pipelines during D3D12CreateDevice). P1 keeps their descriptions;
 * P2 turns them into vkd3d objects. */
typedef struct {
    UINT *code;             /* [program token][length in DWORDs][DXIL ...] */
    SIZE_T code_dwords;
    D3D12DDI_HROOTSIGNATURE root_signature;
} D3D12_UMD_Shader;

typedef struct {
    D3D12DDI_ROOT_SIGNATURE_VERSION version;
    UINT num_parameters;
    UINT num_static_samplers;
    D3D12DDI_ROOT_SIGNATURE_FLAGS flags;
} D3D12_UMD_RootSignature;

typedef struct {
    D3D12DDIARG_CREATE_PIPELINE_STATE_0033 desc;
    bool compute;
} D3D12_UMD_PipelineState;

typedef struct {
    D3D12DDI_BLEND_DESC_0010 desc;
} D3D12_UMD_BlendState;

typedef struct {
    D3D12DDI_DEPTH_STENCIL_DESC_0025 desc;
} D3D12_UMD_DepthStencilState;

typedef struct {
    D3D12DDI_RASTERIZER_DESC_0010 desc;
} D3D12_UMD_RasterizerState;

typedef struct {
    UINT num_elements;
    D3D12DDIARG_INPUT_ELEMENT_DESC *elements;
} D3D12_UMD_ElementLayout;

typedef struct {
    D3D12DDI_COMMAND_POOL_FLAGS flags;
} D3D12_UMD_CommandPool;

typedef struct {
    D3D12DDI_COMMAND_QUEUE_FLAGS queue_flags;
    D3D12DDI_HCOMMANDPOOL_0040 pool;
} D3D12_UMD_CommandRecorder;

/* d3d12_adapter.c */
bool d3d12_umd_is_dwm(void);

/* d3d12_caps.c */
HRESULT APIENTRY d3d12_umd_get_caps(D3D12DDI_HADAPTER hAdapter, const D3D12DDIARG_GETCAPS *pArgs);
D3D12DDI_3DPIPELINELEVEL d3d12_umd_pipeline_level(const D3D12_UMD_Adapter *adapter);

/* d3d12_device.c */
SIZE_T APIENTRY d3d12_umd_calc_private_device_size(D3D12DDI_HADAPTER hAdapter, const D3D12DDIARG_CALCPRIVATEDEVICESIZE *pArgs);
HRESULT APIENTRY d3d12_umd_create_device(D3D12DDI_HADAPTER hAdapter, const D3D12DDIARG_CREATEDEVICE_0003 *pArgs);
void APIENTRY d3d12_umd_destroy_device(D3D12DDI_HDEVICE hDevice);

/* d3d12_tables.c */
void d3d12_umd_fill_core(D3D12DDI_DEVICE_FUNCS_CORE_0040 *t);
void d3d12_umd_fill_command_list(D3D12DDI_COMMAND_LIST_FUNCS_3D_0040 *t);
void d3d12_umd_fill_command_queue(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *t);
void d3d12_umd_fill_extended_features(D3D12DDI_EXTENDED_FEATURES_FUNCS_0021 *t);

/* ddi_stubs.c (generated by gen_ddi_stubs.py) */
void d3d12_umd_stub_hit(const char *table, unsigned slot, const char *name, volatile LONG *hits);
void d3d12_umd_stub_fill_core(D3D12DDI_DEVICE_FUNCS_CORE_0040 *t);
void d3d12_umd_stub_fill_cl(D3D12DDI_COMMAND_LIST_FUNCS_3D_0040 *t);
void d3d12_umd_stub_fill_queue(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *t);
void d3d12_umd_stub_fill_ext(D3D12DDI_EXTENDED_FEATURES_FUNCS_0021 *t);
