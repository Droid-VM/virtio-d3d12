// Ordinary D3D9 imports: this executable never calls a 9On12 creation API.
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../d3d9-proxy/9on12_compat.h"

static void check(HRESULT hr, const char *what) {
    if (FAILED(hr)) { printf("FAIL %s hr=%08lx\n",what,(unsigned long)hr); exit(1); }
}
#define CHECK(x) check((x),#x)
static void require(bool ok, const char *what) {
    printf("%s=%s\n",what,ok ? "PASS" : "FAIL");
    if (!ok) exit(1);
}
static DWORD WINAPI perf_thread(void *) {
    D3DPERF_BeginEvent(0xff123456,L"proxy lazy forwarding");
    D3DPERF_SetMarker(0xff654321,L"marker");
    D3DPERF_SetRegion(0xff123456,L"region");
    D3DPERF_GetStatus(); D3DPERF_QueryRepeatFrame(); D3DPERF_EndEvent();
    return 0;
}
static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcW(w,m,wp,lp); }
static void draw(IDirect3DDevice9 *dev) {
    struct Vertex { float x,y,z,w; DWORD color; };
    const Vertex triangle[] = {{32,224,0.5f,1,0xff4080c0},{128,32,0.5f,1,0xff4080c0},{224,224,0.5f,1,0xff4080c0}};
    CHECK(dev->SetRenderState(D3DRS_LIGHTING,FALSE));
    CHECK(dev->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));
    CHECK(dev->SetRenderState(D3DRS_ZENABLE,FALSE));
    CHECK(dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE));
    CHECK(dev->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1));
    CHECK(dev->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE));
    CHECK(dev->Clear(0,nullptr,D3DCLEAR_TARGET,0xff204060,1,0));
    CHECK(dev->BeginScene());
    CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,triangle,sizeof(Vertex)));
    CHECK(dev->EndScene());
}
int main(int argc, char **argv) {
    setvbuf(stdout,nullptr,_IONBF,0); SetProcessDPIAware();
    bool ex = false, flip = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i],"--ex")) ex = true;
        else if (!strcmp(argv[i],"--flip")) { ex = true; flip = true; }
        else return 2;
    }
    wchar_t module[MAX_PATH] = {}, expected[MAX_PATH] = {};
    HMODULE proxy = GetModuleHandleW(L"d3d9.dll");
    require(proxy != nullptr,"D3D9 imported");
    GetModuleFileNameW(proxy,module,MAX_PATH);
    GetModuleFileNameW(nullptr,expected,MAX_PATH);
    wchar_t *slash = wcsrchr(expected,L'\\'); require(slash != nullptr,"executable directory");
    wcscpy(slash+1,L"d3d9.dll");
    printf("d3d9 import=%ls\n",module);
    require(!_wcsicmp(module,expected),"app-local d3d9 loaded");
    // Race the first lazy load through functions with both scalar and pointer
    // arguments, then exercise the direct-create wrappers.
    HANDLE threads[4];
    for (auto &t : threads) { t = CreateThread(nullptr,0,perf_thread,nullptr,0,nullptr); require(t != nullptr,"create forwarding thread"); }
    require(WaitForMultipleObjects(4,threads,TRUE,10000) == WAIT_OBJECT_0,"concurrent forwarding");
    for (auto t : threads) CloseHandle(t);
    D3DPERF_SetOptions(0);
    require(Direct3DCreate9(0) == nullptr,"invalid SDK rejected");
    for (WORD ordinal = 16; ordinal <= 23; ++ordinal)
        require(GetProcAddress(proxy,MAKEINTRESOURCEA(ordinal)) != nullptr,"ordinal exported");
    IDirect3D9 *d9 = nullptr; IDirect3D9Ex *d9ex = nullptr;
    if (ex) { CHECK(Direct3DCreate9Ex(D3D_SDK_VERSION,&d9ex)); d9 = d9ex; }
    else d9 = Direct3DCreate9(D3D_SDK_VERSION);
    require(d9 != nullptr,"ordinary Direct3DCreate9/Ex");
    UINT ordinal = UINT_MAX;
    for (UINT i = 0; i < d9->GetAdapterCount(); ++i) {
        D3DADAPTER_IDENTIFIER9 id = {}; CHECK(d9->GetAdapterIdentifier(i,0,&id));
        printf("adapter=%s vendor=%04lx\n",id.Description,(unsigned long)id.VendorId);
        if (id.VendorId == 0x1af4) ordinal = i;
    }
    require(ordinal != UINT_MAX,"VirtIO selected");
    WNDCLASSW wc = {}; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"D3D9AppProbe";
    require(RegisterClassW(&wc) != 0,"window class");
    RECT r = {0,0,256,256}; AdjustWindowRect(&r,WS_OVERLAPPEDWINDOW,FALSE);
    HWND window = CreateWindowW(wc.lpszClassName,L"App-local D3D9 proxy",WS_OVERLAPPEDWINDOW,
        100,100,r.right-r.left,r.bottom-r.top,nullptr,nullptr,wc.hInstance,nullptr);
    require(window != nullptr,"window");
    D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.hDeviceWindow = window;
    pp.BackBufferWidth = pp.BackBufferHeight = 256; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.SwapEffect = flip ? D3DSWAPEFFECT_FLIPEX : D3DSWAPEFFECT_DISCARD;
    pp.BackBufferCount = flip ? 2 : 1; pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    IDirect3DDevice9 *dev = nullptr; IDirect3DDevice9Ex *devex = nullptr;
    if (ex) { CHECK(d9ex->CreateDeviceEx(ordinal,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,nullptr,&devex)); dev = devex; }
    else CHECK(d9->CreateDevice(ordinal,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&dev));
    IUnknown *interop = nullptr;
    CHECK(dev->QueryInterface(iid_device9on12,reinterpret_cast<void **>(&interop)));
    interop->Release();
    puts("IDirect3DDevice9On12=PASS");
    bool system9 = false, on12 = false, umd = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,GetCurrentProcessId());
    require(snap != INVALID_HANDLE_VALUE,"module snapshot");
    MODULEENTRY32W m = {}; m.dwSize = sizeof(m);
    if (Module32FirstW(snap,&m)) do {
        if (wcsstr(m.szModule,L"d3d9") || wcsstr(m.szModule,L"dx12um")) printf("loaded=%ls\n",m.szExePath);
        system9 |= !_wcsicmp(m.szModule,L"d3d9.dll") && m.hModule != proxy;
        on12 |= !_wcsicmp(m.szModule,L"d3d9on12.dll");
        umd |= wcsstr(m.szModule,L"dx12um_virtio") != nullptr;
    } while (Module32NextW(snap,&m));
    CloseHandle(snap);
    require(system9 && on12 && umd,"system runtime, 9on12 and hardware UMD");
    ShowWindow(window,SW_SHOW);
    SetWindowPos(window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE | SWP_NOSIZE);
    for (UINT phase = 0; phase < 2; ++phase) {
        for (UINT frame = 0; frame < 60; ++frame) {
            MSG msg; while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            draw(dev);
            if (ex) CHECK(devex->PresentEx(nullptr,nullptr,nullptr,nullptr,0));
            else CHECK(dev->Present(nullptr,nullptr,nullptr,nullptr));
            Sleep(16);
        }
        Sleep(200);
        POINT origin = {}; ClientToScreen(window,&origin);
        HDC dc = GetDC(nullptr);
        COLORREF center = GetPixel(dc,origin.x+128,origin.y+128), corner = GetPixel(dc,origin.x+4,origin.y+4);
        ReleaseDC(nullptr,dc);
        printf("phase=%u center=%06lx corner=%06lx\n",phase,(unsigned long)center,(unsigned long)corner);
        require(center == RGB(0x40,0x80,0xc0) && corner == RGB(0x20,0x40,0x60),"screen pixels");
        if (!phase) {
            if (ex) CHECK(devex->ResetEx(&pp,nullptr));
            else CHECK(dev->Reset(&pp));
            puts("Reset/ResetEx=PASS");
        }
    }
    dev->Release(); d9->Release(); DestroyWindow(window);
    puts("RESULT=PASS"); return 0;
}
