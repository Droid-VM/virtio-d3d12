/* Diagnose DXGI mode enumeration and optionally restore a missing saved mode.
 * --repair-missing validates and persists the active display configuration
 * via SetDisplayConfig. Existing saved configurations are left alone. */
#include <windows.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstring>
#include <vector>

static LONG save_active_configuration()
{
    UINT paths = 0, modes = 0;
    LONG rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes);
    if (rc != ERROR_SUCCESS) return rc;
    std::vector<DISPLAYCONFIG_PATH_INFO> path_info(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> mode_info(modes);
    rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, path_info.data(),
            &modes, mode_info.data(), nullptr);
    if (rc != ERROR_SUCCESS) return rc;
    rc = SetDisplayConfig(paths, path_info.data(), modes, mode_info.data(),
            SDC_VALIDATE | SDC_USE_SUPPLIED_DISPLAY_CONFIG);
    printf("validate_active_configuration=%ld\n", rc);
    if (rc != ERROR_SUCCESS) return rc;
    return SetDisplayConfig(paths, path_info.data(), modes, mode_info.data(),
            SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE);
}

int main(int argc, char **argv)
{
    bool repair = argc == 2 && !strcmp(argv[1], "--repair-missing");
    if (argc != 1 && !repair) {
        fprintf(stderr, "usage: probe_dxgi_modes.exe [--repair-missing]\n");
        return 2;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    IDXGIFactory1 *factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory);
    if (FAILED(hr)) { printf("CreateDXGIFactory1=%08lx\n", hr); return 1; }
    int failures = 0;
    unsigned outputs = 0;
    for (UINT ai = 0;; ++ai) {
        IDXGIAdapter1 *adapter = nullptr;
        hr = factory->EnumAdapters1(ai, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) { ++failures; break; }
        for (UINT oi = 0;; ++oi) {
            IDXGIOutput *output = nullptr;
            hr = adapter->EnumOutputs(oi, &output);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) { ++failures; break; }
            DXGI_OUTPUT_DESC desc = {};
            hr = output->GetDesc(&desc);
            if (FAILED(hr)) { output->Release(); ++failures; continue; }
            if (!desc.AttachedToDesktop) { output->Release(); continue; }
            ++outputs;
            DEVMODEW current = {}, saved = {};
            current.dmSize = sizeof(current);
            saved.dmSize = sizeof(saved);
            BOOL current_ok = EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &current);
            BOOL saved_ok = EnumDisplaySettingsW(desc.DeviceName, ENUM_REGISTRY_SETTINGS, &saved);
            printf("%ls current=%d saved=%d %lux%lu@%lu\n", desc.DeviceName,
                    current_ok, saved_ok, current.dmPelsWidth, current.dmPelsHeight,
                    current.dmDisplayFrequency);
            if (repair && current_ok && !saved_ok) {
                LONG rc = save_active_configuration();
                printf("save_active_configuration=%ld\n", rc);
                if (rc != ERROR_SUCCESS) ++failures;
                saved_ok = EnumDisplaySettingsW(desc.DeviceName, ENUM_REGISTRY_SETTINGS, &saved);
                printf("saved_after=%d %lux%lu@%lu\n", saved_ok,
                        saved.dmPelsWidth, saved.dmPelsHeight, saved.dmDisplayFrequency);
            }
            if (!current_ok || !saved_ok) ++failures;
            bool any_modes = false;
            for (DXGI_FORMAT format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM}) {
                for (UINT flags = 0; flags < 4; ++flags) {
                    UINT count = 0;
                    hr = output->GetDisplayModeList(format, flags, &count, nullptr);
                    printf("format=%u flags=%u hr=%08lx count=%u", format, flags, hr, count);
                    if (SUCCEEDED(hr) && count && count < 65536) {
                        std::vector<DXGI_MODE_DESC> modes(count);
                        hr = output->GetDisplayModeList(format, flags, &count, modes.data());
                        printf(" list_hr=%08lx count=%u", hr, count);
                        if (FAILED(hr) || !count) ++failures;
                        else any_modes = true;
                    } else if (FAILED(hr) || count >= 65536) ++failures;
                    puts("");
                }
            }
            if (!any_modes) ++failures;
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    printf("outputs=%u failures=%d\n", outputs, failures);
    return failures || !outputs ? 1 : 0;
}
