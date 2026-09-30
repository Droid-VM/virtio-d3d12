// Explicit system D3D9On12 smoke test. No app-local graphics runtime required.
#include <windows.h>
#include <d3d9.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

// SDK d3d9on12.h ABI; LLVM-MinGW does not currently ship this header.
struct D3D9ON12_ARGS {
    BOOL Enable9On12;
    IUnknown *pD3D12Device;
    IUnknown *ppD3D12Queues[2];
    UINT NumQueues, NodeMask;
};
struct Device9On12 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetD3D12Device(REFIID, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnwrapUnderlyingResource(IDirect3DResource9 *, ID3D12CommandQueue *, REFIID, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReturnUnderlyingResource(IDirect3DResource9 *, UINT, UINT64 *, ID3D12Fence **) = 0;
};
static const GUID iid9on12 = {0xe7fda234,0xb589,0x4049,{0x94,0x0d,0x88,0x78,0x97,0x75,0x31,0xc8}};
static void check(HRESULT hr, const char *step) {
    printf("%s=0x%08lx\n", step, (unsigned long)hr);
    if (FAILED(hr)) exit(1);
}
#define CHECK(x) check((x), #x)
static void require(bool ok, const char *step) {
    printf("%s=%s\n", step, ok ? "PASS" : "FAIL");
    if (!ok) exit(1);
}
static FARPROC entry(const wchar_t *dll, const char *name) {
    HMODULE module = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    require(module != nullptr, "load system module");
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    printf("module=%ls\n", path);
    FARPROC p = GetProcAddress(module, name);
    require(p != nullptr, name);
    return p; // Keep the module loaded until process exit.
}
static void modules() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    require(snap != INVALID_HANDLE_VALUE, "module snapshot");
    MODULEENTRY32W m = {}; m.dwSize = sizeof(m);
    bool on12 = false, umd = false;
    if (Module32FirstW(snap, &m)) do {
        if (wcsstr(m.szModule, L"d3d") || wcsstr(m.szModule, L"dx12") ||
            wcsstr(m.szModule, L"vulkan")) printf("loaded=%ls\n", m.szExePath);
        on12 |= _wcsicmp(m.szModule, L"d3d9on12.dll") == 0;
        umd |= wcsstr(m.szModule, L"dx12um_virtio") != nullptr;
    } while (Module32NextW(snap, &m));
    CloseHandle(snap);
    require(on12 && umd, "system 9on12 and VirtIO UMD loaded");
}
static LRESULT CALLBACK wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(w, msg, wp, lp);
}
int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool flip = argc == 2 && strcmp(argv[1], "--flip") == 0;
    bool present = flip || (argc == 2 && strcmp(argv[1], "--present") == 0);
    if (argc > 1 && !present) { puts("usage: probe_d3d9on12.exe [--present|--flip]"); return 2; }
    // Load the system runtimes before creating any device.
    auto factoryFn = reinterpret_cast<HRESULT (WINAPI *)(REFIID,void **)>(entry(L"dxgi.dll", "CreateDXGIFactory1"));
    auto deviceFn = reinterpret_cast<HRESULT (WINAPI *)(IUnknown *,D3D_FEATURE_LEVEL,REFIID,void **)>(entry(L"d3d12.dll", "D3D12CreateDevice"));
    auto on12Fn = reinterpret_cast<HRESULT (WINAPI *)(UINT,D3D9ON12_ARGS *,UINT,IDirect3D9Ex **)>(entry(L"d3d9.dll", "Direct3DCreate9On12Ex"));
    ComPtr<IDXGIFactory1> factory;
    CHECK(factoryFn(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(factory.GetAddressOf())));
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc = {};
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> candidate;
        HRESULT hr = factory->EnumAdapters1(i, &candidate);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        CHECK(hr);
        CHECK(candidate->GetDesc1(&desc));
        printf("adapter=%ls vendor=%04x device=%04x luid=%08lx:%08lx flags=%x\n", desc.Description,
            desc.VendorId, desc.DeviceId, (unsigned long)desc.AdapterLuid.HighPart,
            (unsigned long)desc.AdapterLuid.LowPart, desc.Flags);
        if (desc.VendorId == 0x1af4 && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            adapter = candidate; break;
        }
    }
    require(adapter.Get() != nullptr, "VirtIO hardware adapter found");
    ComPtr<ID3D12Device> d12;
    CHECK(deviceFn(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void **>(d12.GetAddressOf())));
    D3D9ON12_ARGS args = {}; args.Enable9On12 = TRUE; args.pD3D12Device = d12.Get();
    ComPtr<IDirect3D9Ex> d9;
    CHECK(on12Fn(D3D_SDK_VERSION, &args, 1, &d9));
    UINT ordinal = UINT_MAX;
    for (UINT i = 0; i < d9->GetAdapterCount(); ++i) {
        LUID luid = {}; CHECK(d9->GetAdapterLUID(i, &luid));
        if (luid.HighPart == desc.AdapterLuid.HighPart && luid.LowPart == desc.AdapterLuid.LowPart) ordinal = i;
    }
    require(ordinal != UINT_MAX, "D3D9 adapter LUID matches VirtIO");
    WNDCLASSW wc = {}; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"VirtIO9On12Probe";
    require(RegisterClassW(&wc) != 0, "RegisterClass");
    HWND window = CreateWindowW(wc.lpszClassName, L"System D3D9On12 / VirtIO", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,400,320,nullptr,nullptr,wc.hInstance,nullptr);
    require(window != nullptr, "CreateWindow");
    D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window; pp.BackBufferWidth = 256; pp.BackBufferHeight = 256;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    if (flip) { pp.SwapEffect = D3DSWAPEFFECT_FLIPEX; pp.BackBufferCount = 2; pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE; }
    ComPtr<IDirect3DDevice9Ex> dev;
    CHECK(d9->CreateDeviceEx(ordinal,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,nullptr,&dev));
    ComPtr<Device9On12> interop;
    CHECK(dev->QueryInterface(iid9on12, reinterpret_cast<void **>(interop.GetAddressOf())));
    ComPtr<IUnknown> underlying, expected;
    CHECK(interop->GetD3D12Device(__uuidof(IUnknown), reinterpret_cast<void **>(underlying.GetAddressOf())));
    CHECK(d12.As(&expected));
    require(underlying.Get() == expected.Get(), "9on12 uses the supplied hardware D3D12 device");
    modules();
    ComPtr<IDirect3DSurface9> rt, readback;
    CHECK(dev->GetRenderTarget(0, &rt));
    CHECK(dev->CreateOffscreenPlainSurface(256,256,D3DFMT_X8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,nullptr));
    auto pixel = [&](UINT x, UINT y, DWORD expectedColor) {
        CHECK(dev->GetRenderTargetData(rt.Get(),readback.Get()));
        D3DLOCKED_RECT locked = {}; CHECK(readback->LockRect(&locked,nullptr,D3DLOCK_READONLY));
        DWORD actual = *reinterpret_cast<DWORD *>(static_cast<BYTE *>(locked.pBits) + y * locked.Pitch + x * 4) & 0xffffff;
        CHECK(readback->UnlockRect());
        printf("pixel(%u,%u)=%06lx expected=%06lx\n",x,y,(unsigned long)actual,(unsigned long)expectedColor);
        require(actual == expectedColor, "pixel check");
    };
    CHECK(dev->Clear(0,nullptr,D3DCLEAR_TARGET,0xff204060,1,0));
    pixel(128,128,0x204060);
    struct Vertex { float x,y,z,rhw; DWORD color; float u,v; };
    Vertex vertices[] = {{32,224,0.5f,1,0xff00ff00,0,1},{128,32,0.5f,1,0xff00ff00,0.5f,0},{224,224,0.5f,1,0xff00ff00,1,1}};
    CHECK(dev->SetRenderState(D3DRS_LIGHTING,FALSE));
    CHECK(dev->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));
    CHECK(dev->SetRenderState(D3DRS_ZENABLE,FALSE));
    CHECK(dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1));
    CHECK(dev->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1));
    CHECK(dev->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE));
    CHECK(dev->BeginScene());
    CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,vertices,sizeof(Vertex)));
    CHECK(dev->EndScene());
    pixel(128,128,0x00ff00); pixel(4,4,0x204060);
    ComPtr<IDirect3DTexture9> texture;
    CHECK(dev->CreateTexture(2,2,1,D3DUSAGE_DYNAMIC,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&texture,nullptr));
    D3DLOCKED_RECT upload = {};
    CHECK(texture->LockRect(0,&upload,nullptr,D3DLOCK_DISCARD));
    for (UINT y = 0; y < 2; ++y) for (UINT x = 0; x < 2; ++x)
        reinterpret_cast<DWORD *>(static_cast<BYTE *>(upload.pBits) + y * upload.Pitch)[x] = 0xff4080c0;
    CHECK(texture->UnlockRect(0));
    CHECK(dev->SetTexture(0,texture.Get()));
    CHECK(dev->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE));
    CHECK(dev->BeginScene());
    CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,vertices,sizeof(Vertex)));
    CHECK(dev->EndScene());
    pixel(128,128,0x4080c0); pixel(4,4,0x204060);
    if (present) {
        ShowWindow(window,SW_SHOW); UpdateWindow(window);
        SetWindowPos(window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE | SWP_NOSIZE);
        for (unsigned frame = 0; frame < 120; ++frame) {
            MSG msg; while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            CHECK(dev->Clear(0,nullptr,D3DCLEAR_TARGET,0xff204060,1,0));
            CHECK(dev->BeginScene());
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,vertices,sizeof(Vertex)));
            CHECK(dev->EndScene());
            CHECK(dev->PresentEx(nullptr,nullptr,nullptr,nullptr,0));
            Sleep(16);
        }
        Sleep(200);
        RECT client = {}; GetClientRect(window,&client);
        HDC dc = GetDC(window);
        COLORREF center = GetPixel(dc,client.right / 2,client.bottom / 2);
        COLORREF corner = GetPixel(dc,4,4);
        ReleaseDC(window,dc);
        printf("window center=%06lx corner=%06lx\n",(unsigned long)center,(unsigned long)corner);
        require(center == RGB(0x40,0x80,0xc0) && corner == RGB(0x20,0x40,0x60), "visible window pixels");
    }
    CHECK(d12->GetDeviceRemovedReason());
    texture.Reset(); readback.Reset(); rt.Reset(); underlying.Reset(); expected.Reset(); interop.Reset(); dev.Reset(); d9.Reset(); d12.Reset();
    DestroyWindow(window);
    puts("RESULT=PASS");
    return 0;
}
