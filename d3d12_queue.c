#include "d3d12_umd.h"

/* ---------------------------------------------------------------------------
 * Command queues and fences (dx12plan.md §2.4 / §8.3).
 *
 * The runtime performs every queue Signal and Wait itself, on the contexts the
 * driver created for the queue with pfnCreateContextCb: a Wait is queued on
 * each of them, a Signal is broadcast to all and completes only when every one
 * reaches it. pfnSignalFence / pfnWaitForFence are never called for monitored
 * fences on a single-node adapter (D3D12Core CCommandQueue::Signal/Wait).
 *
 * vkd3d executes on turnip's own context, so the queue gets one runtime
 * context that gates it from both sides (probe_fence_order.h):
 *
 *  - per ECL, a SignalAtSubmission CPU event: it fires once every earlier
 *    wait on the context is satisfied; until then vkd3d waits on
 *    admit_fence, which the worker signals when it fires;
 *  - behind the ECL, a GPU wait for done_fence >= value, which the worker
 *    CPU-signals when vkd3d's exec_fence reaches that value. A runtime
 *    signal queued later therefore completes only after the work has.
 *
 * One context, not two: the runtime signals a queue by broadcasting
 * D3DKMTSignalSynchronizationObjectFromGpu2 over every context the driver
 * created for it, and the broadcast completes as soon as the FIRST context
 * reaches it (D3D12Core CCastableCommandQueue::SignalImpl; the bcast probe
 * cases). With a second, wait-free context in the list the signal was
 * ungated and application fences completed before the GPU work did. The
 * price is that each admission event sits behind the previous ECL's
 * completion wait, so consecutive ECLs do not overlap on the GPU.
 */

typedef struct {
    UINT64 value;
    HANDLE event;
} D3D12_UMD_Admission;

typedef struct {
    D3D12_UMD_Device *device;
    D3D12DDI_HRTCOMMANDQUEUE hRT;
    ID3D12CommandQueue *vq;
    HANDLE ctx;
    D3DKMT_HANDLE done_fence;
    ID3D12Fence *exec_fence, *admit_fence;

    SRWLOCK lock;
    CONDITION_VARIABLE retired_cv;
    UINT64 submitted; /* value of the last ECL; written by the ECL thread only */
    bool exiting;
    D3D12_UMD_Admission *pending; /* FIFO of gated ECLs, a ring */
    UINT pending_head, pending_count, pending_capacity;
    HANDLE *free_events;
    UINT free_count, free_capacity;

    HANDLE thread, wake, done_event;
    UINT64 signaled; /* done_fence value; worker only */
    UINT64 ecl_count, gated_count;
} D3D12_UMD_Queue;

static HANDLE take_event(D3D12_UMD_Queue *q) {
    HANDLE event = NULL;
    AcquireSRWLockExclusive(&q->lock);
    if (q->free_count)
        event = q->free_events[--q->free_count];
    ReleaseSRWLockExclusive(&q->lock);
    /* Auto-reset: whoever observes the signal also consumes it. */
    return event ? event : CreateEventW(NULL, FALSE, FALSE, NULL);
}

static void put_event(D3D12_UMD_Queue *q, HANDLE event) {
    AcquireSRWLockExclusive(&q->lock);
    if (q->free_count == q->free_capacity) {
        UINT capacity = q->free_capacity ? q->free_capacity * 2 : 8;
        HANDLE *events = realloc(q->free_events, capacity * sizeof(*events));
        if (events) {
            q->free_events = events;
            q->free_capacity = capacity;
        }
    }
    if (q->free_count < q->free_capacity) {
        q->free_events[q->free_count++] = event;
        event = NULL;
    }
    ReleaseSRWLockExclusive(&q->lock);
    if (event)
        CloseHandle(event);
}

/* Caller holds the lock. */
static bool push_admission(D3D12_UMD_Queue *q, UINT64 value, HANDLE event) {
    if (q->pending_count == q->pending_capacity) {
        UINT capacity = q->pending_capacity ? q->pending_capacity * 2 : 16;
        D3D12_UMD_Admission *ring = malloc(capacity * sizeof(*ring));
        if (!ring)
            return false;
        for (UINT i = 0; i < q->pending_count; i++)
            ring[i] = q->pending[(q->pending_head + i) % q->pending_capacity];
        free(q->pending);
        q->pending = ring;
        q->pending_head = 0;
        q->pending_capacity = capacity;
    }
    q->pending[(q->pending_head + q->pending_count++) % q->pending_capacity] = (D3D12_UMD_Admission) { value, event };
    return true;
}

static void signal_done(D3D12_UMD_Queue *q, UINT64 value) {
    D3D12_UMD_Device *device = q->device;
    D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMCPU signal = {
        .ObjectCount = 1,
        .ObjectHandleArray = &q->done_fence,
        .FenceValueArray = &value,
    };
    HRESULT hr = device->kt.pfnSignalSynchronizationObjectFromCpuCb(device->hRTDevice.handle, &signal);
    if (FAILED(hr))
        ERROR("queue %p: signaling done fence to %llu failed: 0x%08lx", q, (unsigned long long)value, hr);
}

static DWORD WINAPI queue_worker(void *param) {
    D3D12_UMD_Queue *q = param;
    UINT64 armed = 0;
    bool drained = false;

    for (;;) {
        UINT64 done = ID3D12Fence_GetCompletedValue(q->exec_fence);
        if (done > q->signaled) {
            signal_done(q, done);
            AcquireSRWLockExclusive(&q->lock);
            q->signaled = done;
            WakeAllConditionVariable(&q->retired_cv);
            ReleaseSRWLockExclusive(&q->lock);
        }

        AcquireSRWLockExclusive(&q->lock);
        UINT64 submitted = q->submitted;
        bool exiting = q->exiting;
        D3D12_UMD_Admission head = { 0 };
        if (q->pending_count && !exiting)
            head = q->pending[q->pending_head];
        ReleaseSRWLockExclusive(&q->lock);

        if (exiting && !drained) {
            /* Nothing can satisfy the runtime waits of a queue being destroyed:
             * let vkd3d run what is left so its fence can reach the end. */
            ID3D12Fence_Signal(q->admit_fence, submitted);
            drained = true;
        }
        if (exiting && done >= submitted)
            break;

        if (done < submitted && armed != done + 1) {
            ID3D12Fence_SetEventOnCompletion(q->exec_fence, done + 1, q->done_event);
            armed = done + 1;
        }

        HANDLE handles[3] = { q->wake, q->done_event, head.event };
        DWORD r = WaitForMultipleObjects(head.event ? 3 : 2, handles, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0 + 2) {
            ID3D12Fence_Signal(q->admit_fence, head.value);
            AcquireSRWLockExclusive(&q->lock);
            q->pending_head = (q->pending_head + 1) % q->pending_capacity;
            q->pending_count--;
            ReleaseSRWLockExclusive(&q->lock);
            put_event(q, head.event);
        } else if (r == WAIT_FAILED) {
            ERROR("queue %p: worker wait failed: %lu", q, GetLastError());
            Sleep(1);
        }
    }
    return 0;
}

/* Hold this ECL until every runtime wait queued before it is satisfied. */
static void admit(D3D12_UMD_Queue *q, UINT64 value) {
    D3D12_UMD_Device *device = q->device;
    HANDLE event = take_event(q);
    if (!event) {
        ERROR("queue %p: no event for admission %llu", q, (unsigned long long)value);
        return;
    }

    D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 signal = {
        .Flags = { .SignalAtSubmission = 1, .EnqueueCpuEvent = 1 },
        .BroadcastContextCount = 1,
        .BroadcastContextArray = &q->ctx,
        .CpuEventHandle = event,
    };
    HRESULT hr = device->kt.pfnSignalSynchronizationObjectFromGpu2Cb(device->hRTDevice.handle, &signal);
    if (FAILED(hr)) {
        ERROR("queue %p: admission signal failed: 0x%08lx", q, hr);
        put_event(q, event);
        return;
    }
    /* No wait pending: the signal has already been processed. */
    if (WaitForSingleObject(event, 0) == WAIT_OBJECT_0) {
        put_event(q, event);
        return;
    }

    ID3D12CommandQueue_Wait(q->vq, q->admit_fence, value);
    AcquireSRWLockExclusive(&q->lock);
    bool queued = push_admission(q, value, event);
    ReleaseSRWLockExclusive(&q->lock);
    if (!queued) {
        /* Cannot track it: admit now rather than never. */
        ERROR("queue %p: admission %llu dropped", q, (unsigned long long)value);
        ID3D12Fence_Signal(q->admit_fence, value);
        put_event(q, event);
        return;
    }
    q->gated_count++;
}

/* Make later work on the context, the runtime's signals included, wait for this ECL. */
static void retire_after(D3D12_UMD_Queue *q, UINT64 value) {
    D3D12_UMD_Device *device = q->device;
    D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait = {
        .hContext = q->ctx,
        .ObjectCount = 1,
        .ObjectHandleArray = &q->done_fence,
        .MonitoredFenceValueArray = &value,
    };
    HRESULT hr = device->kt.pfnWaitForSynchronizationObjectFromGpuCb(device->hRTDevice.handle, &wait);
    if (FAILED(hr))
        ERROR("queue %p: completion wait %llu failed: 0x%08lx", q, (unsigned long long)value, hr);
}

static void APIENTRY execute_command_lists(D3D12DDI_HCOMMANDQUEUE hQueue, UINT count, const D3D12DDI_HCOMMANDLIST *lists) {
    D3D12_UMD_Queue *q = hQueue.pDrvPrivate;
    ID3D12CommandList *stack[16], **vk = stack;
    if (count > ARRAY_SIZE(stack) && !(vk = malloc(count * sizeof(*vk)))) {
        d3d12_umd_set_error(q->device, E_OUTOFMEMORY);
        return;
    }
    for (UINT i = 0; i < count; i++)
        vk[i] = (ID3D12CommandList *)umd_cl(lists[i])->cl;

    UINT64 value = q->submitted + 1;
    /* Native WriteToSubresource can leave a texture persistently mapped.
     * Publish its staging bytes before GPU work consumes the image. */
    HRESULT hr = d3d12_umd_flush_mapped_textures(q->device);
    if (FAILED(hr)) {
        d3d12_umd_set_error(q->device, hr);
        if (vk != stack)
            free(vk);
        return;
    }
    admit(q, value);
    ID3D12CommandQueue_ExecuteCommandLists(q->vq, count, vk);
    ID3D12CommandQueue_Signal(q->vq, q->exec_fence, value);
    retire_after(q, value);

    AcquireSRWLockExclusive(&q->lock);
    q->submitted = value;
    ReleaseSRWLockExclusive(&q->lock);
    SetEvent(q->wake);

    bool synchronous_present = false;
    for (UINT i = 0; i < count; ++i)
        synchronous_present |= umd_cl(lists[i])->synchronous_present;
    if (synchronous_present) {
        /* Legacy blt Present can block in dxgkrnl while holding locks also
         * needed by SignalSynchronizationObjectFromCpu. Retire the present
         * copy and its monitored-fence signal before entering that syscall. */
        AcquireSRWLockExclusive(&q->lock);
        while (q->signaled < value) {
            if (!SleepConditionVariableSRW(&q->retired_cv, &q->lock, 5000, 0)) {
                ReleaseSRWLockExclusive(&q->lock);
                d3d12_umd_set_error(q->device, DXGI_ERROR_DEVICE_HUNG);
                if (vk != stack) free(vk);
                return;
            }
        }
        ReleaseSRWLockExclusive(&q->lock);
    }

    q->ecl_count++;
    if (d3d12_umd_log_enabled())
        INFO("queue %p: ECL %llu (%u lists) returned, %llu gated so far", q, (unsigned long long)value, count,
             (unsigned long long)q->gated_count);
    if (vk != stack)
        free(vk);
}

HANDLE d3d12_umd_present_context(D3D12DDI_HCOMMANDQUEUE hQueue) {
    D3D12_UMD_Queue *q = hQueue.pDrvPrivate;
    return q ? q->ctx : NULL;
}

/* ---------------------------------------------------------------------------
 * Creation.
 */

static D3D12_COMMAND_LIST_TYPE queue_type(D3D12DDI_COMMAND_QUEUE_FLAGS flags) {
    if (flags & D3D12DDI_COMMAND_QUEUE_FLAG_3D)
        return D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (flags & D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE)
        return D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (flags & D3D12DDI_COMMAND_QUEUE_FLAG_COPY)
        return D3D12_COMMAND_LIST_TYPE_COPY;
    return (D3D12_COMMAND_LIST_TYPE)-1;
}

static SIZE_T APIENTRY calc_private_command_queue_size(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATECOMMANDQUEUE_0023 *args) {
    (void)hDevice;
    (void)args;
    return sizeof(D3D12_UMD_Queue);
}

static void release_queue(D3D12_UMD_Queue *q) {
    D3D12_UMD_Device *device = q->device;

    if (q->thread) {
        AcquireSRWLockExclusive(&q->lock);
        q->exiting = true;
        ReleaseSRWLockExclusive(&q->lock);
        SetEvent(q->wake);
        WaitForSingleObject(q->thread, INFINITE);
        CloseHandle(q->thread);
    }
    for (UINT i = 0; i < q->pending_count; i++)
        CloseHandle(q->pending[(q->pending_head + i) % q->pending_capacity].event);
    for (UINT i = 0; i < q->free_count; i++)
        CloseHandle(q->free_events[i]);
    free(q->pending);
    free(q->free_events);

    /* The context after the worker: the last done-fence signal has released
     * every wait queued on it, so destroying it cannot stall. */
    if (q->ctx) {
        D3DDDICB_DESTROYCONTEXT destroy = { .hContext = q->ctx };
        HRESULT hr = device->um->pfnDestroyContextCb(q->hRT, &destroy);
        if (FAILED(hr))
            ERROR("queue %p: DestroyContextCb failed: 0x%08lx", q, hr);
    }
    if (q->done_fence) {
        D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT destroy = { .hSyncObject = q->done_fence };
        device->kt.pfnDestroySynchronizationObjectCb(device->hRTDevice.handle, &destroy);
    }
    if (q->wake)
        CloseHandle(q->wake);
    if (q->done_event)
        CloseHandle(q->done_event);
    if (q->exec_fence)
        ID3D12Fence_Release(q->exec_fence);
    if (q->admit_fence)
        ID3D12Fence_Release(q->admit_fence);
    if (q->vq)
        ID3D12CommandQueue_Release(q->vq);
    memset(q, 0, sizeof(*q));
}

static HRESULT create_context(D3D12_UMD_Queue *q, HANDLE *context) {
    D3DDDICB_CREATECONTEXT create = { 0 };
    HRESULT hr = q->device->um->pfnCreateContextCb(q->hRT, &create);
    if (FAILED(hr))
        return hr;
    *context = create.hContext;
    return S_OK;
}

static HRESULT APIENTRY create_command_queue(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATECOMMANDQUEUE_0023 *args,
                                             D3D12DDI_HCOMMANDQUEUE hQueue, D3D12DDI_HRTCOMMANDQUEUE hRT) {
    D3D12_UMD_Device *device = umd_device(hDevice);
    D3D12_UMD_Queue *q = hQueue.pDrvPrivate;
    memset(q, 0, sizeof(*q));
    q->device = device;
    q->hRT = hRT;
    InitializeSRWLock(&q->lock);

    D3D12_COMMAND_QUEUE_DESC desc = {
        .Type = queue_type(args->QueueFlags),
        .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
        .NodeMask = args->NodeMask,
    };
    if ((int)desc.Type < 0) {
        WARN("%s: queue flags 0x%x not supported", __FUNCTION__, args->QueueFlags);
        return E_INVALIDARG;
    }

    HRESULT hr;
    if (FAILED(hr = ID3D12Device_CreateCommandQueue(device->d3d12, &desc, &IID_ID3D12CommandQueue, (void **)&q->vq)) ||
        FAILED(hr = ID3D12Device_CreateFence(device->d3d12, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
                                             (void **)&q->exec_fence)) ||
        FAILED(hr = ID3D12Device_CreateFence(device->d3d12, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
                                             (void **)&q->admit_fence))) {
        ERROR("%s: vkd3d queue/fence creation failed: 0x%08lx", __FUNCTION__, hr);
        goto fail;
    }

    /* The runtime context can only be created here, on the creating thread. */
    if (FAILED(hr = create_context(q, &q->ctx))) {
        ERROR("%s: CreateContextCb failed: 0x%08lx", __FUNCTION__, hr);
        goto fail;
    }

    D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 fence = {
        .Info.Type = D3DDDI_MONITORED_FENCE,
        .Info.MonitoredFence.InitialFenceValue = 0,
    };
    if (FAILED(hr = device->kt.pfnCreateSynchronizationObject2Cb(device->hRTDevice.handle, &fence))) {
        ERROR("%s: monitored fence creation failed: 0x%08lx", __FUNCTION__, hr);
        goto fail;
    }
    q->done_fence = fence.hSyncObject;

    if (!(q->wake = CreateEventW(NULL, FALSE, FALSE, NULL)) || !(q->done_event = CreateEventW(NULL, FALSE, FALSE, NULL)) ||
        !(q->thread = CreateThread(NULL, 0, queue_worker, q, 0, NULL))) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto fail;
    }

    INFO("%s: queue %p type %d flags 0x%x context %p done fence 0x%x", __FUNCTION__, q, desc.Type,
         args->QueueFlags, q->ctx, q->done_fence);
    return S_OK;

fail:
    release_queue(q);
    return hr;
}

static void APIENTRY destroy_command_queue(D3D12DDI_HDEVICE hDevice, D3D12DDI_HCOMMANDQUEUE hQueue) {
    (void)hDevice;
    D3D12_UMD_Queue *q = hQueue.pDrvPrivate;
    INFO("%s: queue %p, %llu ECLs, %llu gated", __FUNCTION__, q, (unsigned long long)q->ecl_count,
         (unsigned long long)q->gated_count);
    release_queue(q);
}

/* ---------------------------------------------------------------------------
 * Fences: the runtime owns them, signals and waits on them; the driver only
 * receives their GPU addresses.
 */

static SIZE_T APIENTRY calc_private_fence_size(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_CREATE_FENCE *args) {
    (void)hDevice;
    (void)args;
    return sizeof(D3D12_UMD_Fence);
}

static HRESULT APIENTRY create_fence(D3D12DDI_HDEVICE hDevice, D3D12DDI_HFENCE hFence, const D3D12DDIARG_CREATE_FENCE *args) {
    (void)hDevice;
    D3D12_UMD_Fence *fence = hFence.pDrvPrivate;
    memset(fence, 0, sizeof(*fence));
    if (args->FenceCount)
        fence->fence = args->Fences[0];
    if (d3d12_umd_log_enabled())
        INFO("%s: %p count %u value va 0x%llx monitored va 0x%llx flags 0x%x", __FUNCTION__, fence, args->FenceCount,
             (unsigned long long)fence->fence.FenceValue.BaseAddress,
             (unsigned long long)fence->fence.FenceMonitoredValue.BaseAddress, fence->fence.Flags);
    return S_OK;
}

static void APIENTRY destroy_fence(D3D12DDI_HDEVICE hDevice, D3D12DDI_HFENCE hFence) {
    (void)hDevice;
    (void)hFence;
}

/* Only native fences and multi-node adapters reach these. */
static void APIENTRY signal_fence(D3D12DDI_HCOMMANDQUEUE hQueue, D3D12DDIARG_FENCE_OPERATION *op) {
    WARN("%s: queue %p value %llu", __FUNCTION__, hQueue.pDrvPrivate, (unsigned long long)op->Value);
    op->PhysicalAdapterMask = 1;
}

static void APIENTRY wait_for_fence(D3D12DDI_HCOMMANDQUEUE hQueue, D3D12DDIARG_FENCE_OPERATION *op) {
    WARN("%s: queue %p value %llu", __FUNCTION__, hQueue.pDrvPrivate, (unsigned long long)op->Value);
    op->PhysicalAdapterMask = 1;
}

void d3d12_umd_install_queue_core(D3D12DDI_DEVICE_FUNCS_CORE_0040 *t) {
    t->pfnCalcPrivateCommandQueueSize = calc_private_command_queue_size;
    t->pfnCreateCommandQueue = create_command_queue;
    t->pfnDestroyCommandQueue = destroy_command_queue;
    t->pfnCalcPrivateFenceSize = calc_private_fence_size;
    t->pfnCreateFence = create_fence;
    t->pfnDestroyFence = destroy_fence;
}

void d3d12_umd_install_queue(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *t) {
    t->pfnExecuteCommandLists = execute_command_lists;
    t->pfnSignalFence = signal_fence;
    t->pfnWaitForFence = wait_for_fence;
}
