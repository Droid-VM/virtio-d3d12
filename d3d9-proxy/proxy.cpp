#include <windows.h>
#include <cstdint>
#include "9on12_compat.h"
#include "forwarders.h"

static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;
static HMODULE system_d3d9;
static Create9On12 create9;
static Create9On12Ex create9ex;
static FARPROC forwards[sizeof(forward_names) / sizeof(forward_names[0])];

static BOOL CALLBACK initialize(PINIT_ONCE, PVOID, PVOID *) {
    wchar_t path[MAX_PATH];
    UINT length = GetSystemDirectoryW(path,MAX_PATH);
    if (!length || length + ARRAYSIZE(L"\\d3d9.dll") > MAX_PATH) return FALSE;
    lstrcatW(path,L"\\d3d9.dll");
    // An absolute path is essential: a basename load could resolve to this
    // app-local DLL, even when LOAD_LIBRARY_SEARCH_SYSTEM32 is specified.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&initialize),&self);
    HMODULE module = LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module || module == self) return FALSE;
    auto on12 = reinterpret_cast<Create9On12>(GetProcAddress(module,"Direct3DCreate9On12"));
    auto on12ex = reinterpret_cast<Create9On12Ex>(GetProcAddress(module,"Direct3DCreate9On12Ex"));
    if (!on12 || !on12ex) { FreeLibrary(module); SetLastError(ERROR_PROC_NOT_FOUND); return FALSE; }
    for (unsigned i = 0; i < ARRAYSIZE(forwards); ++i)
        forwards[i] = GetProcAddress(module,forward_names[i]);
    create9 = on12;
    create9ex = on12ex;
    system_d3d9 = module; // Retain it for the lifetime of the proxy's function pointers.
    OutputDebugStringA("DroidVM D3D9 proxy: loaded system D3D9; enabling D3D9On12 for Create9/Ex\n");
    return TRUE;
}

static bool ready() {
    return InitOnceExecuteOnce(&init_once,initialize,nullptr,nullptr) && system_d3d9;
}

extern "C" FARPROC resolve_forward(unsigned index) {
    if (ready() && index < ARRAYSIZE(forwards) && forwards[index]) return forwards[index];
    // Never call a missing export with an invented signature or silently
    // pretend that a runtime operation succeeded.
    RaiseFailFastException(nullptr,nullptr,0);
    TerminateProcess(GetCurrentProcess(),ERROR_PROC_NOT_FOUND);
    return nullptr;
}

extern "C" IDirect3D9 *WINAPI proxy_Create9(UINT sdk) {
    if (!ready()) return nullptr;
    D3D9ON12_ARGS args = {}; args.Enable9On12 = TRUE;
    return create9(sdk,&args,1);
}

extern "C" HRESULT WINAPI proxy_Create9Ex(UINT sdk, IDirect3D9Ex **out) {
    if (!out) return D3DERR_INVALIDCALL;
    *out = nullptr;
    if (!ready()) return D3DERR_NOTAVAILABLE;
    D3D9ON12_ARGS args = {}; args.Enable9On12 = TRUE;
    return create9ex(sdk,&args,1,out);
}

// Explicit interoperability callers retain their own overrides and queues.
extern "C" IDirect3D9 *WINAPI proxy_Create9On12(UINT sdk, D3D9ON12_ARGS *args, UINT count) {
    return ready() ? create9(sdk,args,count) : nullptr;
}
extern "C" HRESULT WINAPI proxy_Create9On12Ex(UINT sdk, D3D9ON12_ARGS *args, UINT count, IDirect3D9Ex **out) {
    if (!out) return D3DERR_INVALIDCALL;
    *out = nullptr;
    return ready() ? create9ex(sdk,args,count,out) : D3DERR_NOTAVAILABLE;
}
