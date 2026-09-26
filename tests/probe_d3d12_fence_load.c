/* Measure when an application fence completes after different amounts of GPU
 * work. Run with the system D3D12 runtime on the virtio adapter. */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <stdio.h>

#define BUFFER_SIZE (8u * 1024u * 1024u)

static double elapsed_ms(LARGE_INTEGER start, LARGE_INTEGER end, LARGE_INTEGER frequency)
{
    return (end.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart;
}

int main(void)
{
    IDXGIFactory4 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL;
    ID3D12Device *device = NULL;
    ID3D12CommandQueue *queue = NULL;
    ID3D12CommandAllocator *allocator = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    ID3D12Resource *source = NULL, *target = NULL;
    ID3D12Fence *fence = NULL;
    LARGE_INTEGER frequency, begin, signaled, completed;
    HRESULT hr;
    int result = 1;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (FAILED(hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&factory))) goto done;
    for (UINT i = 0; IDXGIFactory4_EnumAdapters1(factory, i, &adapter) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &desc)) && desc.VendorId == 0x1af4) break;
        IDXGIAdapter1_Release(adapter);
        adapter = NULL;
    }
    if (!adapter) { puts("virtio adapter missing"); goto done; }
    if (FAILED(hr = D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0,
                                     &IID_ID3D12Device, (void **)&device))) goto done;
    D3D12_COMMAND_QUEUE_DESC queue_desc = { .Type = D3D12_COMMAND_LIST_TYPE_COPY };
    if (FAILED(hr = ID3D12Device_CreateCommandQueue(device, &queue_desc,
                                                    &IID_ID3D12CommandQueue, (void **)&queue))) goto done;
    if (FAILED(hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_COPY,
                                                        &IID_ID3D12CommandAllocator, (void **)&allocator))) goto done;
    if (FAILED(hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_COPY, allocator,
                                                   NULL, &IID_ID3D12GraphicsCommandList, (void **)&list))) goto done;
    if (FAILED(hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE,
                                             &IID_ID3D12Fence, (void **)&fence))) goto done;

    D3D12_HEAP_PROPERTIES heap = { .Type = D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC buffer = { .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = BUFFER_SIZE,
        .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1, .SampleDesc = { 1, 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
    if (FAILED(hr = ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_SOURCE, NULL, &IID_ID3D12Resource, (void **)&source))) goto done;
    if (FAILED(hr = ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&target))) goto done;

    QueryPerformanceFrequency(&frequency);
    ID3D12GraphicsCommandList_Close(list);
    unsigned long long value = 0;
    const unsigned counts[] = { 4, 32, 128 };
    for (unsigned trial = 0; trial < 3; ++trial) {
        for (unsigned c = 0; c < ARRAYSIZE(counts); ++c) {
            unsigned copies = counts[c];
            if (FAILED(hr = ID3D12CommandAllocator_Reset(allocator))) goto done;
            if (FAILED(hr = ID3D12GraphicsCommandList_Reset(list, allocator, NULL))) goto done;
            for (unsigned i = 0; i < copies; ++i)
                ID3D12GraphicsCommandList_CopyBufferRegion(list, target, 0, source, 0, BUFFER_SIZE);
            if (FAILED(hr = ID3D12GraphicsCommandList_Close(list))) goto done;
            QueryPerformanceCounter(&begin);
            ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
            ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
            if (FAILED(hr = ID3D12CommandQueue_Signal(queue, fence, ++value))) goto done;
            QueryPerformanceCounter(&signaled);
            unsigned long long immediate = ID3D12Fence_GetCompletedValue(fence);
            do {
                QueryPerformanceCounter(&completed);
                if (elapsed_ms(begin, completed, frequency) > 30000.0) {
                    puts("fence timeout");
                    goto done;
                }
                Sleep(0);
            } while (ID3D12Fence_GetCompletedValue(fence) < value);
            printf("trial=%u copies=%u bytes=%llu immediate=%llu fence=%llu total_ms=%.3f after_signal_ms=%.3f\n",
                   trial, copies, (unsigned long long)copies * BUFFER_SIZE, immediate,
                   (unsigned long long)ID3D12Fence_GetCompletedValue(fence),
                   elapsed_ms(begin, completed, frequency), elapsed_ms(signaled, completed, frequency));
        }
    }
    result = 0;
done:
    if (result && FAILED(hr)) printf("failure hr=0x%08lx\n", (unsigned long)hr);
    if (target) ID3D12Resource_Release(target);
    if (source) ID3D12Resource_Release(source);
    if (fence) ID3D12Fence_Release(fence);
    if (list) ID3D12GraphicsCommandList_Release(list);
    if (allocator) ID3D12CommandAllocator_Release(allocator);
    if (queue) ID3D12CommandQueue_Release(queue);
    if (device) ID3D12Device_Release(device);
    if (adapter) IDXGIAdapter1_Release(adapter);
    if (factory) IDXGIFactory4_Release(factory);
    return result;
}
