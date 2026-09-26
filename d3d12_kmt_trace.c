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

static struct {
    const char *name;
    void *wrapper;
    FARPROC gdi32_address;
} trace_entries[] = {
    { "D3DKMTSignalSynchronizationObjectFromGpu", (void *)wrap_signal_from_gpu },
    { "D3DKMTSignalSynchronizationObjectFromGpu2", (void *)wrap_signal_from_gpu2 },
    { "D3DKMTSignalSynchronizationObjectFromCpu", (void *)wrap_signal_from_cpu },
    { "D3DKMTWaitForSynchronizationObjectFromGpu", (void *)wrap_wait_from_gpu },
    { "D3DKMTWaitForSynchronizationObjectFromCpu", (void *)wrap_wait_from_cpu },
};

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
