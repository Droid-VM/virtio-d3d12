#include "d3d12_umd.h"

#include <errno.h>
#include <virgl_hw.h>

/* The ICD invokes this synchronously from vkAllocateMemory. Keep the result
 * per thread so a concurrent CreateHeapAndResource cannot claim its handles. */
static __thread struct {
    void *hRTResource;
    D3DKMT_HANDLE allocation, resource;
} last_alloc;
static __thread void *primary_rt_resource;

static int runtime_alloc(void *ctx, const VIRTIO_WDDM_RuntimeAllocRequest *req,
                         VIRTIO_WDDM_RuntimeAllocResult *out) {
    D3D12_UMD_Device *device = ctx;
    if (!device || !req || !out || !req->hRTResource || req->hRTResource != primary_rt_resource)
        return -EINVAL;

    VIRTIO_WDDM_CreateResource resource = { .tag = VIRTIO_WDDM_CREATE_RESOURCE_TAG };
    VIRTIO_WDDM_PrimaryAllocation primary = {
        .base = {
            .allocation = { .blob = {
                .tag = VIRTIO_WDDM_ALLOCATE_BLOB_TAG,
                .id = req->blob_id, .mem = req->mem,
                .flags = req->flags, .size = req->size,
            } },
            .reuse_tag = VIRTIO_WDDM_REUSE_BLOB_TAG,
            .source = req->sourceAllocation,
        },
        .primary_tag = VIRTIO_WDDM_PRIMARY_ALLOCATION_TAG,
        /* DDI 0040's CREATEHEAP has no primary mode descriptor. */
        .refresh_numerator = 60,
        .refresh_denominator = 1,
        .vidpn_source = 0,
    };
    DEVMODEW mode = { .dmSize = sizeof(mode) };
    if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &mode) &&
        mode.dmDisplayFrequency && mode.dmDisplayFrequency <= 1000)
        primary.refresh_numerator = mode.dmDisplayFrequency;
    D3D12DDI_ALLOCATION_INFO_0022 info = {
        .pPrivateDriverData = &primary,
        .PrivateDriverDataSize = sizeof(primary),
        .VidPnSourceId = 0,
        .Flags = D3D12DDI_ALLOCATION_INFO_FLAGS_0022_PRIMARY,
    };
    D3D12DDICB_ALLOCATE_0022 allocate = {
        .pPrivateDriverData = &resource,
        .PrivateDriverDataSize = sizeof(resource),
        .hResource = req->hRTResource,
        .NumAllocations = 1,
        .pAllocationInfo = &info,
    };
    if (!device->um22 || !device->um22->pfnAllocateCb)
        return -ENOSYS;
    HRESULT hr = device->um22->pfnAllocateCb(device->hRTDevice, &allocate);
    if (FAILED(hr) || !info.hAllocation) {
        ERROR("primary AllocateCb failed: 0x%08lx", hr);
        return -EIO;
    }
    *out = (VIRTIO_WDDM_RuntimeAllocResult) {
        .hAllocation = info.hAllocation,
        .hKMResource = allocate.hKMResource,
    };
    last_alloc.hRTResource = req->hRTResource;
    last_alloc.allocation = info.hAllocation;
    last_alloc.resource = allocate.hKMResource;
    return 0;
}

static int runtime_free(void *ctx, const VIRTIO_WDDM_RuntimeAllocResult *alloc) {
    D3D12_UMD_Device *device = ctx;
    if (!device || !alloc)
        return -EINVAL;
    if (!alloc->hAllocation)
        return 0;
    /* vkFreeMemory closes the ICD BO. D3D12's AllocateCb associates this
     * allocation with hRTResource and rejects allocation-level DeallocateCb
     * (E_INVALIDARG); destroy_primary retires the resource after the BO and
     * VkImage are gone. Keep last_alloc for CreateHeapAndResource's failure
     * cleanup when vkAllocateMemory frees the BO before returning an error. */
    return 0;
}

void d3d12_umd_init_runtime_allocator(D3D12_UMD_Device *device) {
    device->runtime_allocator = (VkD3DDDIRuntimeAllocator) {
        .sType = VK_STRUCTURE_TYPE_D3DDDI_RUNTIME_ALLOCATOR,
        .ctx = device,
        .alloc = (void *)runtime_alloc,
        .free = (void *)runtime_free,
    };
}

HRESULT d3d12_umd_init_present_device(D3D12_UMD_Device *device) {
    if (!device->um22 || !device->um22->pfnAllocateCb || !device->um22->pfnDeallocateCb)
        return E_NOTIMPL;
    struct vkd3d_instance *instance = vkd3d_instance_from_device(device->d3d12);
    device->vk_instance = vkd3d_instance_get_vk_instance(instance);
    device->vk_physical = vkd3d_get_vk_physical_device(device->d3d12);
    device->vk_device = vkd3d_get_vk_device(device->d3d12);
    device->vk_gdpa = (PFN_vkGetDeviceProcAddr)device->vk_bootstrap.vk_GetInstanceProcAddr(
        device->vk_instance, "vkGetDeviceProcAddr");
    if (!device->vk_gdpa)
        return E_FAIL;

    INFO("D3D12 allocation callbacks=%p/%p residency=%p paging=%p",
         device->um22->pfnAllocateCb, device->um22->pfnDeallocateCb,
         device->kt.pfnMakeResidentCb, device->kt.pfnCreatePagingQueueCb);

    D3DDDICB_CREATEPAGINGQUEUE paging = {
        .Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL,
        .PhysicalAdapterIndex = 0,
    };
    HRESULT hr = device->kt.pfnCreatePagingQueueCb(device->hRTDevice.handle, &paging);
    if (SUCCEEDED(hr)) {
        device->paging.queue = paging.hPagingQueue;
        device->paging.sync_object = paging.hSyncObject;
    }
    return hr;
}

void d3d12_umd_release_present_device(D3D12_UMD_Device *device) {
    if (device->paging.queue) {
        D3DDDI_DESTROYPAGINGQUEUE paging = { .hPagingQueue = device->paging.queue };
        device->kt.pfnDestroyPagingQueueCb(device->hRTDevice.handle, &paging);
    }
    memset(&device->paging, 0, sizeof(device->paging));
}

static VkFormat vk_format(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        default:
            return VK_FORMAT_UNDEFINED;
    }
}

static UINT virgl_format(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
            return VIRGL_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return VIRGL_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return VIRGL_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return VIRGL_FORMAT_R16G16B16A16_FLOAT;
        default:
            return VIRGL_FORMAT_NONE;
    }
}

static HRESULT make_resident(D3D12_UMD_Device *device, D3DKMT_HANDLE allocation) {
    if (!device->kt.pfnMakeResidentCb)
        return E_NOTIMPL;
    D3DDDI_MAKERESIDENT args = {
        .hPagingQueue = device->paging.queue,
        .NumAllocations = 1,
        .AllocationList = &allocation,
        .Flags = { .CantTrimFurther = 1, .MustSucceed = 1 },
    };
    HRESULT hr = device->kt.pfnMakeResidentCb(device->hRTDevice.handle, &args);
    if (hr == E_PENDING) {
        D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {
            .ObjectCount = 1,
            .ObjectHandleArray = &device->paging.sync_object,
            .FenceValueArray = &args.PagingFenceValue,
        };
        hr = device->kt.pfnWaitForSynchronizationObjectFromCpuCb(device->hRTDevice.handle, &wait);
    }
    return hr;
}

HRESULT d3d12_umd_create_primary(D3D12_UMD_Device *device, D3D12_UMD_Resource *resource,
                                  D3D12DDI_HRTRESOURCE hRTResource) {
    const D3D12_RESOURCE_DESC *desc = &resource->desc;
    if (!hRTResource.handle || desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        !desc->Width || !desc->Height || desc->DepthOrArraySize != 1 ||
        desc->SampleDesc.Count != 1 || vk_format(desc->Format) == VK_FORMAT_UNDEFINED)
        return E_INVALIDARG;

#define VK_PROC(name) ((PFN_vk##name)device->vk_gdpa(device->vk_device, "vk" #name))
    PFN_vkCreateImage create_image = VK_PROC(CreateImage);
    PFN_vkGetImageMemoryRequirements2 get_requirements = VK_PROC(GetImageMemoryRequirements2);
    PFN_vkGetPhysicalDeviceMemoryProperties2 get_memory_properties =
        (PFN_vkGetPhysicalDeviceMemoryProperties2)device->vk_bootstrap.vk_GetInstanceProcAddr(
            device->vk_instance, "vkGetPhysicalDeviceMemoryProperties2");
    PFN_vkAllocateMemory allocate_memory = VK_PROC(AllocateMemory);
    PFN_vkBindImageMemory2 bind_memory = VK_PROC(BindImageMemory2);
    PFN_vkGetImageSubresourceLayout get_layout = VK_PROC(GetImageSubresourceLayout);
#undef VK_PROC
    if (!create_image || !get_requirements || !get_memory_properties ||
        !allocate_memory || !bind_memory || !get_layout)
        return E_FAIL;

    VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT,
    };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external,
        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = vk_format(desc->Format),
        .extent = { desc->Width, desc->Height, 1 },
        .mipLevels = desc->MipLevels ? desc->MipLevels : 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult vr = create_image(device->vk_device, &image_info, NULL, &resource->primary_image);
    if (vr != VK_SUCCESS)
        return E_OUTOFMEMORY;

    VkImageMemoryRequirementsInfo2 req_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = resource->primary_image,
    };
    VkMemoryRequirements2 req = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    get_requirements(device->vk_device, &req_info, &req);
    VkPhysicalDeviceMemoryProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
    };
    get_memory_properties(device->vk_physical, &props);
    UINT type = 0;
    while (type < props.memoryProperties.memoryTypeCount &&
           !(req.memoryRequirements.memoryTypeBits & (1u << type)))
        type++;
    if (type == props.memoryProperties.memoryTypeCount)
        return E_OUTOFMEMORY;

    VkD3DDDICreateResource d3d_create = {
        .sType = VK_STRUCTURE_TYPE_D3DDDI_CREATE_RESOURCE,
        .hRTResource = hRTResource.handle,
    };
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &d3d_create,
        .image = resource->primary_image,
    };
    VkExportMemoryAllocateInfo export = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT,
    };
    VkMemoryAllocateInfo mem_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export,
        .allocationSize = req.memoryRequirements.size,
        .memoryTypeIndex = type,
    };
    memset(&last_alloc, 0, sizeof(last_alloc));
    void *previous = primary_rt_resource;
    primary_rt_resource = hRTResource.handle;
    vr = allocate_memory(device->vk_device, &mem_info, NULL, &resource->primary_memory);
    primary_rt_resource = previous;
    if (last_alloc.hRTResource == hRTResource.handle && last_alloc.allocation) {
        resource->hRTResource = hRTResource;
        resource->hKMAllocation = last_alloc.allocation;
        resource->hKMResource = last_alloc.resource;
    }
    if (vr != VK_SUCCESS || !resource->hKMAllocation)
        return E_OUTOFMEMORY;

    VkBindImageMemoryInfo bind = {
        .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
        .image = resource->primary_image,
        .memory = resource->primary_memory,
    };
    if (bind_memory(device->vk_device, 1, &bind) != VK_SUCCESS)
        return E_OUTOFMEMORY;
    HRESULT hr = make_resident(device, resource->hKMAllocation);
    if (FAILED(hr))
        return hr;

    VkImageSubresource sub = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT };
    VkSubresourceLayout layout;
    get_layout(device->vk_device, resource->primary_image, &sub, &layout);
    if (!layout.rowPitch || layout.rowPitch > UINT32_MAX || layout.offset > UINT32_MAX)
        return E_INVALIDARG;
    VIRTIO_WDDM_BlobInfoSet blob = {
        .tag = VIRTIO_WDDM_ESCAPE_BLOB_INFO_SET_TAG,
        .handle = resource->hKMAllocation,
        .blob_info = {
            .width = desc->Width, .height = desc->Height,
            .format = virgl_format(desc->Format),
            .bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW |
                    VIRGL_BIND_DISPLAY_TARGET | VIRGL_BIND_SCANOUT,
            .modifier = 0,
            .dxgi_format = desc->Format,
        },
    };
    blob.blob_info.strides[0] = layout.rowPitch;
    blob.blob_info.offsets[0] = layout.offset;
    D3DDDICB_ESCAPE escape = {
        .hDevice = device->hRTDevice.handle,
        .pPrivateDriverData = &blob,
        .PrivateDriverDataSize = sizeof(blob),
    };
    hr = device->kt.pfnEscapeCb(device->adapter->hRTAdapter.handle, &escape);
    if (FAILED(hr))
        return hr;

    struct vkd3d_image_resource_create_info wrap = {
        .vk_image = resource->primary_image,
        .desc = *desc,
        .present_state = D3D12_RESOURCE_STATE_PRESENT,
    };
    hr = vkd3d_create_image_resource(device->d3d12, &wrap, &resource->res);
    if (SUCCEEDED(hr))
        INFO("primary created allocation=0x%x resource=0x%x %llux%u format=%u pitch=%u",
             resource->hKMAllocation, resource->hKMResource, (unsigned long long)desc->Width, desc->Height,
             desc->Format, blob.blob_info.strides[0]);
    return hr;
}

void d3d12_umd_destroy_primary(D3D12_UMD_Device *device, D3D12_UMD_Resource *resource) {
    if (!resource->primary_image && !resource->primary_memory && !resource->hKMAllocation)
        return;
    PFN_vkDestroyImage destroy_image = (PFN_vkDestroyImage)device->vk_gdpa(device->vk_device, "vkDestroyImage");
    PFN_vkFreeMemory free_memory = (PFN_vkFreeMemory)device->vk_gdpa(device->vk_device, "vkFreeMemory");
    if (resource->primary_image)
        destroy_image(device->vk_device, resource->primary_image, NULL);
    if (resource->primary_memory)
        free_memory(device->vk_device, resource->primary_memory, NULL);
    if (resource->hKMAllocation) {
        D3D12DDICB_DEALLOCATE_0022 deallocate = { .hResource = resource->hRTResource.handle };
        HRESULT hr = device->um22->pfnDeallocateCb(device->hRTDevice, &deallocate);
        if (FAILED(hr))
            ERROR("primary deallocation 0x%x failed: 0x%08lx", resource->hKMAllocation, hr);
    }
    resource->primary_image = VK_NULL_HANDLE;
    resource->primary_memory = VK_NULL_HANDLE;
    resource->hKMAllocation = 0;
}
