#include "d3d12_umd.h"

#include <d3dkmthk.h>
#include <tlhelp32.h>

/* ---------------------------------------------------------------------------
 * Runtime sync-call tracer (VIRTIO_WDDM_LOG_KMT=1, log file on). The D3D12
 * runtime turns queue Signal/Wait into D3DKMT calls of its own inside
 * d3d12core; showing exactly which operations it queues against the runtime
 * context the UMD created is what the fence ordering of dx12plan §2.4 needs.
 *
 * d3d12core imports these entry points from the D3DKMT API set through
 * DELAY imports by ordinal, so both the plain import scan and a name match
 * find nothing. Ordinals are mapped to names by comparing what the API set
 * resolves them to with what gdi32 resolves each name to, and the delay
 * IAT slot is rewritten with the wrapper directly: the eager resolution
 * here replaces the delay helper for those five entries.
 */

static LONG trace_installed;
static LARGE_INTEGER trace_start;

static double trace_ms(void) {
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return (double)(now.QuadPart - trace_start.QuadPart) * 1000.0 / (double)frequency.QuadPart;
}

static NTSTATUS APIENTRY wrap_signal_from_gpu(D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU *args) {
    if (d3d12_umd_log_enabled())
        INFO("%.3f kmt SignalFromGpu: ctx 0x%lx objs %u first 0x%x val %llu", trace_ms(), (unsigned long)args->hContext,
             args->ObjectCount, args->ObjectCount ? args->ObjectHandleArray[0] : 0,
             (unsigned long long)(args->ObjectCount ? args->MonitoredFenceValueArray[0] : 0));
    return D3DKMTSignalSynchronizationObjectFromGpu(args);
}

static NTSTATUS APIENTRY wrap_signal_from_gpu2(D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 *args) {
    if (d3d12_umd_log_enabled()) {
        char contexts[80] = "";
        for (UINT i = 0; i < args->BroadcastContextCount && i < 4; i++)
            snprintf(contexts + strlen(contexts), sizeof(contexts) - strlen(contexts), "%s0x%lx",
                     i ? " " : "", (unsigned long)args->BroadcastContextArray[i]);
        INFO("%.3f kmt SignalFromGpu2: flags 0x%x%s%s%s objs %u first 0x%x val %llu ctx(%lu) [%s] cpu_event %p",
             trace_ms(), *(const UINT *)&args->Flags,
             args->Flags.SignalAtSubmission ? " SignalAtSubmission" : "",
             args->Flags.EnqueueCpuEvent ? " EnqueueCpuEvent" : "",
             args->Flags.AllowFenceRewind ? " AllowFenceRewind" : "",
             args->ObjectCount, args->ObjectCount ? args->ObjectHandleArray[0] : 0,
             (unsigned long long)(args->ObjectCount ? args->MonitoredFenceValueArray[0] : 0),
             args->BroadcastContextCount, contexts, args->CpuEventHandle);
    }
    return D3DKMTSignalSynchronizationObjectFromGpu2(args);
}

static NTSTATUS APIENTRY wrap_signal_from_cpu(D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU *args) {
    if (d3d12_umd_log_enabled())
        INFO("%.3f kmt SignalFromCpu: dev 0x%lx objs %u first 0x%x val %llu flags 0x%x", trace_ms(), (unsigned long)args->hDevice,
             args->ObjectCount, args->ObjectCount ? args->ObjectHandleArray[0] : 0,
             (unsigned long long)(args->ObjectCount ? args->FenceValueArray[0] : 0),
             *(const UINT *)&args->Flags);
    return D3DKMTSignalSynchronizationObjectFromCpu(args);
}

static NTSTATUS APIENTRY wrap_wait_from_gpu(D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU *args) {
    if (d3d12_umd_log_enabled())
        INFO("%.3f kmt WaitFromGpu: ctx 0x%lx objs %u first 0x%x val %llu", trace_ms(), (unsigned long)args->hContext,
             args->ObjectCount, args->ObjectCount ? args->ObjectHandleArray[0] : 0,
             (unsigned long long)(args->ObjectCount ? args->MonitoredFenceValueArray[0] : 0));
    return D3DKMTWaitForSynchronizationObjectFromGpu(args);
}

static NTSTATUS APIENTRY wrap_wait_from_cpu(D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *args) {
    if (d3d12_umd_log_enabled())
        INFO("%.3f kmt WaitFromCpu: dev 0x%lx objs %u first 0x%x val %llu", trace_ms(), (unsigned long)args->hDevice,
             args->ObjectCount, args->ObjectCount ? args->ObjectHandleArray[0] : 0,
             (unsigned long long)(args->ObjectCount ? args->FenceValueArray[0] : 0));
    return D3DKMTWaitForSynchronizationObjectFromCpu(args);
}
/* Allocation/lock path: Map(NULL) fails here somewhere; log the calls. */
static NTSTATUS APIENTRY wrap_create_allocation(D3DKMT_CREATEALLOCATION *a) {
    if (d3d12_umd_log_enabled())
        INFO("%.3f kmt CreateAllocation: dev 0x%lx res 0x%lx count %u flags 0x%x", trace_ms(),
             (unsigned long)a->hDevice, (unsigned long)a->hResource, a->NumAllocations, *(const UINT *)&a->Flags);
    NTSTATUS s = D3DKMTCreateAllocation(a);
    if (d3d12_umd_log_enabled())
        INFO("%.3f kmt CreateAllocation: status 0x%lx", trace_ms(), (unsigned long)s);
    return s;
}

static NTSTATUS APIENTRY wrap_lock(D3DKMT_LOCK *a) {
    if (d3d12_umd_log_enabled())
        INFO("%.3f kmt Lock: alloc 0x%lx flags 0x%x", trace_ms(), (unsigned long)a->hAllocation, *(const UINT *)&a->Flags);
    return D3DKMTLock(a);
}


static struct {
    const char *name;
    void *wrapper;
    FARPROC gdi32_address;
} trace_entries[] = {
    { "D3DKMTSignalSynchronizationObjectFromGpu", (void *)wrap_signal_from_gpu },
    { "D3DKMTCreateAllocation", (void *)wrap_create_allocation },
    { "D3DKMTLock", (void *)wrap_lock },
    { "D3DKMTSignalSynchronizationObjectFromGpu2", (void *)wrap_signal_from_gpu2 },
    { "D3DKMTSignalSynchronizationObjectFromCpu", (void *)wrap_signal_from_cpu },
    { "D3DKMTWaitForSynchronizationObjectFromGpu", (void *)wrap_wait_from_gpu },
    { "D3DKMTWaitForSynchronizationObjectFromCpu", (void *)wrap_wait_from_cpu },
};

/* Enumerate every D3DKMT function the runtime imports by ordinal: the
 * resolved gdi32 address identifies the name. */
static const char *const kmt_names[] = {
    "D3DKMTAcquireKeyedMutex",
    "D3DKMTAcquireKeyedMutex2",
    "D3DKMTAdjustFullscreenGamma",
    "D3DKMTCancelPresents",
    "D3DKMTChangeSurfacePointer",
    "D3DKMTChangeVideoMemoryReservation",
    "D3DKMTCheckExclusiveOwnership",
    "D3DKMTCheckMonitorPowerState",
    "D3DKMTCheckMultiPlaneOverlaySupport",
    "D3DKMTCheckMultiPlaneOverlaySupport2",
    "D3DKMTCheckMultiPlaneOverlaySupport3",
    "D3DKMTCheckOcclusion",
    "D3DKMTCheckSharedResourceAccess",
    "D3DKMTCheckVidPnExclusiveOwnership",
    "D3DKMTCloseAdapter",
    "D3DKMTCloseDxCoreDevice",
    "D3DKMTConfigureSharedResource",
    "D3DKMTConnectDoorbell",
    "D3DKMTCreateAllocation",
    "D3DKMTCreateAllocation2",
    "D3DKMTCreateContext",
    "D3DKMTCreateContextVirtual",
    "D3DKMTCreateDCFromMemory",
    "D3DKMTCreateDevice",
    "D3DKMTCreateDoorbell",
    "D3DKMTCreateHwContext",
    "D3DKMTCreateHwQueue",
    "D3DKMTCreateHwQueueForUserModeSubmission",
    "D3DKMTCreateKeyedMutex",
    "D3DKMTCreateKeyedMutex2",
    "D3DKMTCreateNativeFence",
    "D3DKMTCreateOutputDupl",
    "D3DKMTCreateOverlay",
    "D3DKMTCreatePagingQueue",
    "D3DKMTCreateProtectedSession",
    "D3DKMTCreateSyncFile",
    "D3DKMTCreateSynchronizationObject",
    "D3DKMTCreateSynchronizationObject2",
    "D3DKMTDestroyAllocation",
    "D3DKMTDestroyAllocation2",
    "D3DKMTDestroyContext",
    "D3DKMTDestroyDCFromMemory",
    "D3DKMTDestroyDevice",
    "D3DKMTDestroyDoorbell",
    "D3DKMTDestroyHwContext",
    "D3DKMTDestroyHwQueue",
    "D3DKMTDestroyKeyedMutex",
    "D3DKMTDestroyOutputDupl",
    "D3DKMTDestroyOverlay",
    "D3DKMTDestroyPagingQueue",
    "D3DKMTDestroyProtectedSession",
    "D3DKMTDestroySynchronizationObject",
    "D3DKMTEnumAdapters",
    "D3DKMTEnumAdapters2",
    "D3DKMTEnumAdapters3",
    "D3DKMTEscape",
    "D3DKMTEvict",
    "D3DKMTFlipOverlay",
    "D3DKMTFlushHeapTransitions",
    "D3DKMTFreeGpuVirtualAddress",
    "D3DKMTGetAllocationPriority",
    "D3DKMTGetContextInProcessSchedulingPriority",
    "D3DKMTGetContextSchedulingPriority",
    "D3DKMTGetDWMVerticalBlankEvent",
    "D3DKMTGetDeviceState",
    "D3DKMTGetDisplayModeList",
    "D3DKMTGetMultiPlaneOverlayCaps",
    "D3DKMTGetMultisampleMethodList",
    "D3DKMTGetNativeFenceLogDetail",
    "D3DKMTGetOverlayState",
    "D3DKMTGetPostCompositionCaps",
    "D3DKMTGetPresentHistory",
    "D3DKMTGetPresentQueueEvent",
    "D3DKMTGetProcessDeviceRemovalSupport",
    "D3DKMTGetProcessSchedulingPriorityClass",
    "D3DKMTGetResourcePresentPrivateDriverData",
    "D3DKMTGetRuntimeData",
    "D3DKMTGetScanLine",
    "D3DKMTGetSharedPrimaryHandle",
    "D3DKMTGetSharedResourceAdapterLuid",
    "D3DKMTInvalidateActiveVidPn",
    "D3DKMTInvalidateCache",
    "D3DKMTIsFeatureEnabled",
    "D3DKMTLock",
    "D3DKMTLock2",
    "D3DKMTMakeResident",
    "D3DKMTMapGpuVirtualAddress",
    "D3DKMTMarkDeviceAsError",
    "D3DKMTNotifyWorkSubmission",
    "D3DKMTOfferAllocations",
    "D3DKMTOpenAdapterFromDeviceName",
    "D3DKMTOpenAdapterFromGdiDisplayName",
    "D3DKMTOpenAdapterFromHdc",
    "D3DKMTOpenAdapterFromLuid",
    "D3DKMTOpenKeyedMutex",
    "D3DKMTOpenKeyedMutex2",
    "D3DKMTOpenKeyedMutexFromNtHandle",
    "D3DKMTOpenNativeFenceFromNtHandle",
    "D3DKMTOpenNtHandleFromName",
    "D3DKMTOpenProtectedSessionFromNtHandle",
    "D3DKMTOpenResource",
    "D3DKMTOpenResource2",
    "D3DKMTOpenResourceFromNtHandle",
    "D3DKMTOpenSyncObjectFromNtHandle",
    "D3DKMTOpenSyncObjectFromNtHandle2",
    "D3DKMTOpenSyncObjectFromSyncFile",
    "D3DKMTOpenSyncObjectNtHandleFromName",
    "D3DKMTOpenSynchronizationObject",
    "D3DKMTOutputDuplGetFrameInfo",
    "D3DKMTOutputDuplGetMetaData",
    "D3DKMTOutputDuplGetPointerShapeData",
    "D3DKMTOutputDuplPresent",
    "D3DKMTOutputDuplPresentToHwQueue",
    "D3DKMTOutputDuplReleaseFrame",
    "D3DKMTPollDisplayChildren",
    "D3DKMTPresent",
    "D3DKMTPresentMultiPlaneOverlay",
    "D3DKMTPresentMultiPlaneOverlay2",
    "D3DKMTPresentMultiPlaneOverlay3",
    "D3DKMTPresentRedirected",
    "D3DKMTQueryAdapterInfo",
    "D3DKMTQueryAllocationResidency",
    "D3DKMTQueryClockCalibration",
    "D3DKMTQueryFSEBlock",
    "D3DKMTQueryFeatureInterface",
    "D3DKMTQueryProcessOfferInfo",
    "D3DKMTQueryProtectedSessionInfoFromNtHandle",
    "D3DKMTQueryProtectedSessionStatus",
    "D3DKMTQueryRemoteVidPnSourceFromGdiDisplayName",
    "D3DKMTQueryResourceInfo",
    "D3DKMTQueryResourceInfoFromNtHandle",
    "D3DKMTQueryStatistics",
    "D3DKMTQueryVidPnExclusiveOwnership",
    "D3DKMTQueryVideoMemoryInfo",
    "D3DKMTReclaimAllocations",
    "D3DKMTReclaimAllocations2",
    "D3DKMTRegisterTrimNotification",
    "D3DKMTRegisterVailProcess",
    "D3DKMTReleaseKeyedMutex",
    "D3DKMTReleaseKeyedMutex2",
    "D3DKMTReleaseProcessVidPnSourceOwners",
    "D3DKMTRender",
    "D3DKMTReserveGpuVirtualAddress",
    "D3DKMTResizeRingBuffer",
    "D3DKMTSetAllocationPriority",
    "D3DKMTSetContextInProcessSchedulingPriority",
    "D3DKMTSetContextSchedulingPriority",
    "D3DKMTSetDisplayMode",
    "D3DKMTSetDisplayPrivateDriverFormat",
    "D3DKMTSetFSEBlock",
    "D3DKMTSetGammaRamp",
    "D3DKMTSetHwProtectionTeardownRecovery",
    "D3DKMTSetMonitorColorSpaceTransform",
    "D3DKMTSetProcessSchedulingPriorityClass",
    "D3DKMTSetQueuedLimit",
    "D3DKMTSetStablePowerState",
    "D3DKMTSetSyncRefreshCountWaitTarget",
    "D3DKMTSetVidPnSourceHwProtection",
    "D3DKMTSetVidPnSourceOwner",
    "D3DKMTSetVidPnSourceOwner1",
    "D3DKMTSetVidPnSourceOwner2",
    "D3DKMTShareObjectWithHost",
    "D3DKMTShareObjects",
    "D3DKMTSharedPrimaryLockNotification",
    "D3DKMTSharedPrimaryUnLockNotification",
    "D3DKMTSignalSynchronizationObject",
    "D3DKMTSignalSynchronizationObject2",
    "D3DKMTSignalSynchronizationObjectFromCpu",
    "D3DKMTSignalSynchronizationObjectFromGpu",
    "D3DKMTSignalSynchronizationObjectFromGpu2",
    "D3DKMTSubmitCommand",
    "D3DKMTSubmitCommandToHwQueue",
    "D3DKMTSubmitPresentBltToHwQueue",
    "D3DKMTSubmitPresentToHwQueue",
    "D3DKMTSubmitSignalSyncObjectsToHwQueue",
    "D3DKMTSubmitWaitForSyncObjectsToHwQueue",
    "D3DKMTTrimProcessCommitment",
    "D3DKMTUnlock",
    "D3DKMTUnlock2",
    "D3DKMTUnregisterTrimNotification",
    "D3DKMTUpdateAllocationProperty",
    "D3DKMTUpdateGpuVirtualAddress",
    "D3DKMTUpdateOverlay",
    "D3DKMTWaitForIdle",
    "D3DKMTWaitForSynchronizationObject",
    "D3DKMTWaitForSynchronizationObject2",
    "D3DKMTWaitForSynchronizationObjectFromCpu",
    "D3DKMTWaitForSynchronizationObjectFromGpu",
    "D3DKMTWaitForVerticalBlankEvent",
    "D3DKMTWaitForVerticalBlankEvent2",
    "D3DKMTWaitSyncFile",
};


static const char *kmt_name_for(FARPROC resolved) {
    HMODULE gdi = GetModuleHandleA("gdi32.dll");
    if (!gdi)
        return NULL;
    for (size_t i = 0; i < ARRAY_SIZE(kmt_names); i++)
        if (GetProcAddress(gdi, kmt_names[i]) == resolved)
            return kmt_names[i];
    return NULL;
}

static bool is_d3dkmt_api_set(const char *dll) {
    return strstr(dll, "d3dkmt") != NULL;
}

/* One delay-import descriptor's name and import thunks, with the older
 * absolute-address form normalized to RVAs. */
typedef struct {
    const char *name;
    IMAGE_THUNK_DATA *iat; /* FirstThunk: what calls go through */
    IMAGE_THUNK_DATA *int_; /* OriginalFirstThunk: ordinals */
} delay_import;

static bool patch_delay_import(BYTE *base, const delay_import *import, const char *module_name) {
    HMODULE api = LoadLibraryA(import->name);
    if (!api)
        return false;

    bool patched = false;
    for (IMAGE_THUNK_DATA *names = import->int_, *slots = import->iat; names->u1.AddressOfData; names++, slots++) {
        if (!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
            continue;
        FARPROC resolved = GetProcAddress(api, (LPCSTR)(uintptr_t)IMAGE_ORDINAL(names->u1.Ordinal));
        if (!resolved)
            continue;
        if (d3d12_umd_log_enabled()) {
            const char *n = kmt_name_for(resolved);
            INFO("kmt delay import ordinal %u -> %s", (unsigned)IMAGE_ORDINAL(names->u1.Ordinal), n ? n : "?");
        }
        for (size_t e = 0; e < ARRAY_SIZE(trace_entries); e++) {
            if (resolved != trace_entries[e].gdi32_address)
                continue;
            DWORD old;
            if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old))
                continue;
            slots->u1.Function = (ULONG_PTR)trace_entries[e].wrapper;
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old, &old);
            patched = true;
        }
    }
    if (patched)
        INFO("%s: patched delay sync imports of %s from %s", __FUNCTION__, module_name, import->name);
    return patched;
}

static bool patch_module(BYTE *base, const char *module_name) {
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (!dir.VirtualAddress)
        return false;

    bool patched = false;
    /* ImgDelayDescr: grAttrs, name, module handle, IAT, INT, bound, unload, stamp. */
    for (const DWORD *d = (const DWORD *)(base + dir.VirtualAddress); d[0] || d[1]; d += 8) {
        bool rva_form = (d[0] & 1) != 0;
        if (!d[1])
            continue;
        delay_import import = {
            .name = (const char *)(base + (rva_form ? d[1] : d[1] - (UINT_PTR)base)),
            .iat = (IMAGE_THUNK_DATA *)(base + (rva_form ? d[3] : d[3] - (UINT_PTR)base)),
            .int_ = (IMAGE_THUNK_DATA *)(base + (rva_form ? d[4] : d[4] - (UINT_PTR)base)),
        };
        if (!is_d3dkmt_api_set(import.name) || !d[4])
            continue;
        patched |= patch_delay_import(base, &import, module_name);
    }
    return patched;
}

extern IMAGE_DOS_HEADER __ImageBase;

void d3d12_umd_kmt_trace_init(void) {
    char setting[2] = {0};
    DWORD length = GetEnvironmentVariableA("VIRTIO_WDDM_LOG_KMT", setting, sizeof(setting));
    if (!(length == 1 && setting[0] == '1'))
        return;
    if (InterlockedCompareExchange(&trace_installed, 1, 0))
        return;
    QueryPerformanceCounter(&trace_start);

    HMODULE gdi = GetModuleHandleA("gdi32.dll");
    for (size_t e = 0; e < ARRAY_SIZE(trace_entries); e++)
        trace_entries[e].gdi32_address = gdi ? GetProcAddress(gdi, trace_entries[e].name) : NULL;

    /* d3d12core and friends are loaded before OpenAdapter12 reaches here. */
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    HMODULE self = (HMODULE)&__ImageBase;
    MODULEENTRY32W entry = { .dwSize = sizeof(entry) };
    unsigned patched = 0, modules = 0;
    for (BOOL ok = Module32FirstW(snapshot, &entry); ok; ok = Module32NextW(snapshot, &entry)) {
        if ((HMODULE)entry.modBaseAddr == self)
            continue;
        char name[MAX_PATH];
        WideCharToMultiByte(CP_ACP, 0, entry.szModule, -1, name, sizeof(name), NULL, NULL);
        modules++;
        if (patch_module(entry.modBaseAddr, name))
            patched++;
    }
    CloseHandle(snapshot);
    INFO("%s: %u module(s) scanned, %u patched", __FUNCTION__, modules, patched);
}
