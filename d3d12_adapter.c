#include "d3d12_umd.h"

#include <d3dkmthk.h>

/* ---------------------------------------------------------------------------
 * Logging (VIRTIO_WDDM_LOG_FILE / VIRTIO_WDDM_VERBOSE, as in dx11um).
 */

static volatile LONG log_state; /* 0 = pending, 1 = off, 2 = file */
static FILE *log_file;

static bool verbose_enabled(void) {
    static volatile LONG cached;
    LONG value = InterlockedCompareExchange(&cached, 0, 0);
    if (!value) {
        char setting[2] = {0};
        DWORD length = GetEnvironmentVariableA("VIRTIO_WDDM_VERBOSE", setting, sizeof(setting));
        value = length == 1 && setting[0] == '1' ? 2 : 1;
        InterlockedCompareExchange(&cached, value, 0);
    }
    return value == 2;
}

static LONG log_init(void) {
    LONG value = InterlockedCompareExchange(&log_state, 0, 0);
    if (value)
        return value;

    char path[MAX_PATH] = {0};
    DWORD len = GetEnvironmentVariableA("VIRTIO_WDDM_LOG_FILE", path, sizeof(path));
    FILE *f = NULL;
    if (len > 0 && len < sizeof(path)) {
        if (len == 1 && path[0] == '1')
            strcpy(path, "C:\\dx12um_log.txt");
        if (!(len == 1 && path[0] == '0'))
            f = fopen(path, "a+");
    }
    if (InterlockedCompareExchange(&log_state, f ? 2 : 1, 0) == 0)
        log_file = f;
    else if (f)
        fclose(f);
    return InterlockedCompareExchange(&log_state, 0, 0);
}

bool d3d12_umd_log_enabled(void) {
    return log_init() == 2 || verbose_enabled();
}

static void log_raw(const char *line, bool debugger) {
    LONG state = log_init();
    if (state == 1 && !debugger && !verbose_enabled())
        return;

    char tagged[4608];
    int len = snprintf(tagged, sizeof(tagged), "[dx12um pid=%lu tid=%lu] %s",
                       GetCurrentProcessId(), GetCurrentThreadId(), line);
    if (len < 0)
        return;
    tagged[sizeof(tagged) - 1] = '\0';

    if (debugger || verbose_enabled())
        OutputDebugStringA(tagged);

    if (state == 2 && log_file) {
        fwrite(tagged, strlen(tagged), 1, log_file);
        fflush(log_file);
    }
}

static void vlog(bool debugger, const char *file, int line, const char *label, const char *format, va_list args) {
    char buf[4096];
    const char *base = strrchr(file, '/');
    int n = snprintf(buf, sizeof(buf), "%s %s:%d: ", label, base ? base + 1 : file, line);
    if (n < 0 || (size_t)n >= sizeof(buf))
        n = 0;
    vsnprintf(buf + n, sizeof(buf) - n, format, args);
    size_t len = strlen(buf);
    if (len > sizeof(buf) - 2)
        len = sizeof(buf) - 2;
    buf[len] = '\n';
    buf[len + 1] = '\0';
    log_raw(buf, debugger);
}

void virtio_wddm_log(const char *file, int line, const char *label, const char *format, ...) {
    va_list args;
    va_start(args, format);
    /* Errors also reach a debugger: they are rare, and silent failures inside
     * dwm.exe are otherwise invisible. */
    vlog(!strcmp(label, "ERROR"), file, line, label, format, args);
    va_end(args);
}

void d3d12_umd_stub_hit(const char *table, unsigned slot, const char *name, volatile LONG *hits) {
    LONG n = InterlockedIncrement(hits);
    /* First hit of every slot, then powers of two. */
    if (n == 1 || !(n & (n - 1))) {
        char line[256];
        snprintf(line, sizeof(line), "STUB %s[%u] %s called (%ld)\n", table, slot, name, n);
        log_raw(line, true);
    }
}

/* ---------------------------------------------------------------------------
 * Registry switches.
 */

bool d3d12_umd_is_dwm(void) {
    WCHAR path[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, path, ARRAY_SIZE(path));
    if (!len || len >= ARRAY_SIZE(path))
        return false;
    const WCHAR *base = wcsrchr(path, L'\\');
    return !_wcsicmp(base ? base + 1 : path, L"dwm.exe");
}

/* The adapter's software key through the runtime (WDDM 2.4+ callback). */
static bool query_dword_cb2(const D3DDDI_ADAPTERCALLBACKS *callbacks, HANDLE hRTAdapter,
                            const WCHAR *name, DWORD *value) {
    if (!callbacks->pfnQueryAdapterInfoCb2)
        return false;

    D3DDDI_QUERYREGISTRY_INFO info = {
        .QueryType = D3DDDI_QUERYREGISTRY_ADAPTERKEY,
        .ValueType = REG_DWORD,
    };
    wcsncpy(info.ValueName, name, ARRAY_SIZE(info.ValueName) - 1);
    D3DDDICB_QUERYADAPTERINFO2 query = {
        .QueryType = D3DDDI_QUERYADAPTERTYPE_QUERYREGISTRY,
        .pPrivateDriverData = &info,
        .PrivateDriverDataSize = sizeof(info),
    };
    HRESULT hr = callbacks->pfnQueryAdapterInfoCb2(hRTAdapter, &query);
    if (FAILED(hr) || info.Status != D3DDDI_QUERYREGISTRY_STATUS_SUCCESS) {
        INFO("%s: %ls hr=0x%08lx status=%d", __FUNCTION__, name, hr, info.Status);
        return false;
    }
    *value = info.OutputDword;
    return true;
}

typedef NTSTATUS (APIENTRY *PFN_OPENADAPTERFROMLUID)(D3DKMT_OPENADAPTERFROMLUID *);
typedef NTSTATUS (APIENTRY *PFN_QUERYADAPTERINFO)(const D3DKMT_QUERYADAPTERINFO *);
typedef NTSTATUS (APIENTRY *PFN_CLOSEADAPTER)(const D3DKMT_CLOSEADAPTER *);

/* The same query straight through dxgkrnl, for runtimes without the callback. */
static bool query_dword_kmt(LUID luid, const WCHAR *name, DWORD *value) {
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi)
        return false;
    PFN_OPENADAPTERFROMLUID open = (PFN_OPENADAPTERFROMLUID)GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid");
    PFN_QUERYADAPTERINFO query = (PFN_QUERYADAPTERINFO)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
    PFN_CLOSEADAPTER close = (PFN_CLOSEADAPTER)GetProcAddress(gdi, "D3DKMTCloseAdapter");
    if (!open || !query || !close)
        return false;

    D3DKMT_OPENADAPTERFROMLUID args = { .AdapterLuid = luid };
    if (open(&args) < 0)
        return false;

    D3DDDI_QUERYREGISTRY_INFO info = {
        .QueryType = D3DDDI_QUERYREGISTRY_ADAPTERKEY,
        .ValueType = REG_DWORD,
    };
    wcsncpy(info.ValueName, name, ARRAY_SIZE(info.ValueName) - 1);
    D3DKMT_QUERYADAPTERINFO qai = {
        .hAdapter = args.hAdapter,
        .Type = KMTQAITYPE_QUERYREGISTRY,
        .pPrivateDriverData = &info,
        .PrivateDriverDataSize = sizeof(info),
    };
    NTSTATUS status = query(&qai);
    D3DKMT_CLOSEADAPTER closing = { .hAdapter = args.hAdapter };
    close(&closing);
    if (status < 0 || info.Status != D3DDDI_QUERYREGISTRY_STATUS_SUCCESS) {
        INFO("%s: %ls status=0x%08lx query=%d", __FUNCTION__, name, (unsigned long)status, info.Status);
        return false;
    }
    *value = info.OutputDword;
    return true;
}

static DWORD adapter_dword(const D3DDDI_ADAPTERCALLBACKS *callbacks, HANDLE hRTAdapter, const LUID *luid,
                           const WCHAR *name, DWORD fallback) {
    DWORD value;
    if (query_dword_cb2(callbacks, hRTAdapter, name, &value))
        return value;
    if (luid && query_dword_kmt(*luid, name, &value))
        return value;
    return fallback;
}

/* ---------------------------------------------------------------------------
 * Adapter table.
 */

static HRESULT APIENTRY d3d12_umd_close_adapter(D3D12DDI_HADAPTER hAdapter) {
    TRACE_CALL();
    free(hAdapter.pDrvPrivate);
    return S_OK;
}

static HRESULT APIENTRY d3d12_umd_get_supported_versions(D3D12DDI_HADAPTER hAdapter, UINT32 *puEntries,
                                                        UINT64 *pSupportedDDIInterfaceVersions) {
    (void)hAdapter;
    static const UINT64 versions[] = { D3D12DDI_SUPPORTED_0040 };

    /* Count first (NULL array), then fill. */
    if (pSupportedDDIInterfaceVersions) {
        UINT32 n = *puEntries < ARRAY_SIZE(versions) ? *puEntries : ARRAY_SIZE(versions);
        memcpy(pSupportedDDIInterfaceVersions, versions, n * sizeof(*versions));
    }
    *puEntries = ARRAY_SIZE(versions);
    INFO("%s: %u version(s), filled=%d", __FUNCTION__, *puEntries, pSupportedDDIInterfaceVersions != NULL);
    return S_OK;
}

static HRESULT APIENTRY d3d12_umd_get_optional_ddi_tables(D3D12DDI_HADAPTER hAdapter, UINT32 *puEntries,
                                                         D3D12DDI_TABLE_REQUEST *pRequests) {
    (void)hAdapter;
    (void)pRequests;
    /* Only extra COMMAND_LIST_3D tables could be asked for; none needed. */
    *puEntries = 0;
    return S_OK;
}

/* Write a filled table into the runtime's buffer, never past its size: the
 * size follows the negotiated version, not our struct. */
static void copy_table(const char *name, void *dst, SIZE_T size, const void *src, SIZE_T src_size) {
    if (size != src_size)
        INFO("fill %s: runtime size %zu, table size %zu", name, (size_t)size, (size_t)src_size);
    memcpy(dst, src, size < src_size ? size : src_size);
    if (size > src_size)
        memset((char *)dst + src_size, 0, size - src_size);
}

static HRESULT APIENTRY d3d12_umd_fill_ddi_table(D3D12DDI_HADAPTER hAdapter, D3D12DDI_TABLE_TYPE type, void *pTable,
                                                SIZE_T size, UINT index, D3D12DDI_HRTTABLE hRTTable) {
    D3D12_UMD_Adapter *adapter = umd_adapter(hAdapter);
    INFO("%s: type=%d size=%zu index=%u rttable=%p", __FUNCTION__, type, (size_t)size, index, hRTTable.handle);

    switch (type) {
    case D3D12DDI_TABLE_TYPE_DEVICE_CORE: {
        D3D12DDI_DEVICE_FUNCS_CORE_0040 t;
        d3d12_umd_fill_core(&t);
        copy_table("core", pTable, size, &t, sizeof(t));
        return S_OK;
    }
    case D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D: {
        D3D12DDI_COMMAND_LIST_FUNCS_3D_0040 t;
        d3d12_umd_fill_command_list(&t);
        copy_table("command list", pTable, size, &t, sizeof(t));
        if (index < ARRAY_SIZE(adapter->cl_tables))
            adapter->cl_tables[index] = hRTTable;
        return S_OK;
    }
    case D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D: {
        D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 t;
        d3d12_umd_fill_command_queue(&t);
        copy_table("command queue", pTable, size, &t, sizeof(t));
        return S_OK;
    }
    case D3D12DDI_TABLE_TYPE_0020_EXTENDED_FEATURES: {
        /* 24 bytes is the _0020 shape, 32 the _0021 one; the first three
         * slots are common. */
        D3D12DDI_EXTENDED_FEATURES_FUNCS_0021 t;
        d3d12_umd_fill_extended_features(&t);
        copy_table("extended features", pTable, size, &t, sizeof(t));
        return S_OK;
    }
    default:
        /* Video, content protection, metacommands, render passes, shader
         * cache ...: not offered at 0040. */
        INFO("%s: table type %d not supported", __FUNCTION__, type);
        return E_INVALIDARG;
    }
}

__attribute__((visibility("default"))) HRESULT APIENTRY OpenAdapter12(D3D12DDIARG_OPENADAPTER *pArgs) {
    const D3DDDI_ADAPTERCALLBACKS *callbacks = pArgs->pAdapterCallbacks;
    HANDLE hRTAdapter = pArgs->hRTAdapter.handle;

    /* The switch is read before anything else is touched: while it is off the
     * runtime must see exactly what it saw before this DLL existed. dwm.exe
     * calls OpenAdapter12 too, so it needs its own opt-in. */
    DWORD enable = adapter_dword(callbacks, hRTAdapter, NULL, L"D3D12Enable", D3D12_UMD_ENABLE_APPS);
    bool dwm = d3d12_umd_is_dwm();
    INFO("%s: D3D12Enable=%lu dwm=%d", __FUNCTION__, enable, dwm);
    if (enable == D3D12_UMD_ENABLE_OFF || (enable == D3D12_UMD_ENABLE_APPS && dwm))
        return DXGI_ERROR_UNSUPPORTED;

    d3d12_umd_kmt_trace_init();

    VIRTIO_WDDM_AdapterCaps caps;
    HRESULT hr = virtio_wddm_query_adapter(callbacks, hRTAdapter, &caps);
    if (FAILED(hr))
        return hr;

    D3D12_UMD_Adapter *adapter = calloc(1, sizeof(*adapter));
    if (!adapter)
        return E_OUTOFMEMORY;

    adapter->hRTAdapter = pArgs->hRTAdapter;
    adapter->callbacks = *callbacks;
    adapter->caps = caps;
    adapter->feature_level_override = adapter_dword(callbacks, hRTAdapter, &caps.luid,
                                                    L"D3D12FeatureLevelOverride", 0);

    pArgs->hAdapter.pDrvPrivate = adapter;
    pArgs->pAdapterFuncs->pfnCalcPrivateDeviceSize = d3d12_umd_calc_private_device_size;
    pArgs->pAdapterFuncs->pfnCreateDevice = d3d12_umd_create_device;
    pArgs->pAdapterFuncs->pfnCloseAdapter = d3d12_umd_close_adapter;
    pArgs->pAdapterFuncs->pfnGetSupportedVersions = d3d12_umd_get_supported_versions;
    pArgs->pAdapterFuncs->pfnGetCaps = d3d12_umd_get_caps;
    pArgs->pAdapterFuncs->pfnGetOptionalDDITables = d3d12_umd_get_optional_ddi_tables;
    pArgs->pAdapterFuncs->pfnFillDDITable = d3d12_umd_fill_ddi_table;
    pArgs->pAdapterFuncs->pfnDestroyDevice = d3d12_umd_destroy_device;

    INFO("%s: adapter %p luid %08lx:%08lx feature level override 0x%lx", __FUNCTION__, adapter,
         caps.luid.HighPart, caps.luid.LowPart, adapter->feature_level_override);
    return S_OK;
}
