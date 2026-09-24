#include "d3d12_umd.h"

/* ---------------------------------------------------------------------------
 * vkd3d bridge.
 *
 * vkd3d-proton creates its own VkInstance through the vkGetInstanceProcAddr it
 * is given, so there is no way to hand it pNext structures. Give it a
 * vkGetInstanceProcAddr whose vkCreateInstance prepends the direct-loading
 * list and VkD3DDDICallbacks chain (the same one dx11um passes) and forwards
 * everything else to the package loader.
 */

static PFN_vkGetInstanceProcAddr loader_gipa;
/* The device whose instance is being created on this thread. */
static __thread D3D12_UMD_Device *creating_device;

static VKAPI_ATTR VkResult VKAPI_CALL bridge_vkCreateInstance(const VkInstanceCreateInfo *info,
                                                               const VkAllocationCallbacks *allocator,
                                                               VkInstance *instance) {
    D3D12_UMD_Device *device = creating_device;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)loader_gipa(NULL, "vkCreateInstance");
    if (!device || !create) {
        ERROR("%s: no device being created (%p) or loader (%p)", __FUNCTION__, device, create);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkBaseOutStructure *tail = (VkBaseOutStructure *)&device->vk_bootstrap.loading_list;
    while (tail->pNext)
        tail = tail->pNext;
    tail->pNext = (VkBaseOutStructure *)info->pNext;

    VkInstanceCreateInfo patched = *info;
    patched.pNext = &device->vk_bootstrap.loading_list;
    VkResult res = create(&patched, allocator, instance);

    tail->pNext = NULL;
    INFO("%s: vkCreateInstance %d instance=%p", __FUNCTION__, res, res == VK_SUCCESS ? *instance : NULL);
    return res;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL bridge_vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    if (!instance && !strcmp(name, "vkCreateInstance"))
        return (PFN_vkVoidFunction)bridge_vkCreateInstance;
    return loader_gipa(instance, name);
}

static HRESULT create_vkd3d_device(D3D12_UMD_Device *device) {
    static const char *const instance_extensions[] = { VK_LUNARG_DIRECT_DRIVER_LOADING_EXTENSION_NAME };
    const struct vkd3d_instance_create_info instance_info = {
        .pfn_vkGetInstanceProcAddr = bridge_vkGetInstanceProcAddr,
        .instance_extensions = instance_extensions,
        .instance_extension_count = ARRAY_SIZE(instance_extensions),
    };

    /* Every bootstrap loads the same package loader, so one pointer serves
     * all devices; vkd3d keeps calling through it after creation. */
    InterlockedExchangePointer((void **)&loader_gipa, device->vk_bootstrap.vk_GetInstanceProcAddr);

    struct vkd3d_instance *instance = NULL;
    creating_device = device;
    HRESULT hr = vkd3d_create_instance(&instance_info, &instance);
    creating_device = NULL;
    if (FAILED(hr)) {
        ERROR("%s: vkd3d_create_instance failed: 0x%08lx", __FUNCTION__, hr);
        return hr;
    }

    const struct vkd3d_device_create_info device_info = {
        .minimum_feature_level = D3D_FEATURE_LEVEL_11_0,
        .instance = instance,
        .adapter_luid = device->adapter->caps.luid,
        /* One vkd3d device per runtime device: the runtime implements the
         * API's per-adapter singleton itself. */
        .independent = true,
    };
    hr = vkd3d_create_device(&device_info, &IID_ID3D12Device, (void **)&device->d3d12);
    /* The device holds its own reference to the instance. */
    vkd3d_instance_decref(instance);
    if (FAILED(hr)) {
        ERROR("%s: vkd3d_create_device failed: 0x%08lx", __FUNCTION__, hr);
        return hr;
    }
    return S_OK;
}

/* ---------------------------------------------------------------------------
 * Device creation.
 */

#define DEVICE_PRIVATE_SIZE 4096
static_assert(sizeof(D3D12_UMD_Device) <= DEVICE_PRIVATE_SIZE);

SIZE_T APIENTRY d3d12_umd_calc_private_device_size(D3D12DDI_HADAPTER hAdapter,
                                                   const D3D12DDIARG_CALCPRIVATEDEVICESIZE *pArgs) {
    D3D12_UMD_Adapter *adapter = umd_adapter(hAdapter);
    adapter->interface_version = pArgs->Interface;
    adapter->version = pArgs->Version;
    INFO("%s: interface=0x%x version=0x%x flags=0x%x", __FUNCTION__, pArgs->Interface, pArgs->Version, pArgs->Flags);
    return DEVICE_PRIVATE_SIZE;
}

static void release_device(D3D12_UMD_Device *device) {
    if (device->d3d12) {
        ULONG refs = ID3D12Device_Release(device->d3d12);
        INFO("%s: released vkd3d device, refs=%lu", __FUNCTION__, refs);
        device->d3d12 = NULL;
    }
    virtio_wddm_vk_bootstrap_release(&device->vk_bootstrap);
}

/* Give the runtime's device a host context of its own, as dx11um does:
 * allocations made through the runtime callbacks land on it. */
static HRESULT init_runtime_context(D3D12_UMD_Device *device) {
    if (!(device->adapter->caps.capsets & VIRTIO_WDDM_CAPSET_MASK_DRM))
        return S_OK;
    VIRTIO_WDDM_ContextInit init = {
        .tag = VIRTIO_WDDM_ESCAPE_CONTEXT_INIT_TAG,
        .capset_id = VIRTIO_WDDM_CAPSET_ID_DRM,
        .num_rings = 1,
        .debug_name = "d3d12-runtime",
    };
    D3DDDICB_ESCAPE escape = {
        .hDevice = device->hRTDevice.handle,
        .pPrivateDriverData = &init,
        .PrivateDriverDataSize = sizeof(init),
    };
    HRESULT hr = device->kt.pfnEscapeCb(device->adapter->hRTAdapter.handle, &escape);
    INFO("%s: runtime ContextInit rtdev=%p hr=0x%08lx", __FUNCTION__, device->hRTDevice.handle, hr);
    return hr;
}

HRESULT APIENTRY d3d12_umd_create_device(D3D12DDI_HADAPTER hAdapter, const D3D12DDIARG_CREATEDEVICE_0003 *pArgs) {
    D3D12_UMD_Adapter *adapter = umd_adapter(hAdapter);
    D3D12_UMD_Device *device = pArgs->hDrvDevice.pDrvPrivate;
    memset(device, 0, sizeof(*device));

    INFO("%s: process=%lu interface=0x%x version=0x%x flags=0x%x", __FUNCTION__, GetCurrentProcessId(),
         pArgs->Interface, pArgs->Version, pArgs->Flags);

    device->adapter = adapter;
    device->hRTDevice = pArgs->hRTDevice;
    device->kt = *pArgs->pKTCallbacks;
    device->um = pArgs->p12UMCallbacks;
    device->flags = pArgs->Flags;

    device->callbacks = (VkD3DDDICallbacks) {
        .sType = VK_STRUCTURE_TYPE_D3DDDI_CALLBACKS,
        .AdapterLuid = adapter->caps.luid,
        .hRTAdapter = adapter->hRTAdapter.handle,
        .hRTDevice = device->hRTDevice.handle,
        .pAdapterCallbacks = &adapter->callbacks,
        .pKTCallbacks = &device->kt,
    };

    HRESULT hr = virtio_wddm_vk_bootstrap(&device->vk_bootstrap, adapter->caps.capsets,
                                          adapter->hRTAdapter.handle, device->hRTDevice.handle,
                                          &device->kt, &device->callbacks);
    if (FAILED(hr))
        return hr;

    if (FAILED(hr = create_vkd3d_device(device)) || FAILED(hr = init_runtime_context(device))) {
        release_device(device);
        return hr;
    }

    for (UINT i = 0; i < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; i++)
        device->descriptor_size[i] = ID3D12Device_GetDescriptorHandleIncrementSize(device->d3d12, i);

    INFO("%s: device %p vkd3d %p", __FUNCTION__, device, device->d3d12);
    return S_OK;
}

void APIENTRY d3d12_umd_destroy_device(D3D12DDI_HDEVICE hDevice) {
    D3D12_UMD_Device *device = umd_device(hDevice);
    INFO("%s: device %p", __FUNCTION__, device);
    release_device(device);
    memset(device, 0, sizeof(*device));
}
