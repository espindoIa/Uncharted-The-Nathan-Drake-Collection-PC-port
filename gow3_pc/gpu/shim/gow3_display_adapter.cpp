// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_display_adapter.h"

#ifdef _WIN32
#include <cstring>
#include <windows.h>
#include <dxgi.h>
#endif

namespace Gow3DisplayAdapter {

Result Compare(void* hwnd, const std::uint8_t* luid) {
#ifdef _WIN32
    if (!hwnd || !luid) {
        return Result::Unknown;
    }
    const HMONITOR monitor = MonitorFromWindow(static_cast<HWND>(hwnd), MONITOR_DEFAULTTONEAREST);
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return Result::Unknown;
    }
    Result result = Result::Unknown;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; result == Result::Unknown &&
                     factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND;
         ++a) {
        IDXGIOutput* output = nullptr;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            const bool shows_window = SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor;
            output->Release();
            if (shows_window) {
                DXGI_ADAPTER_DESC1 adapter_desc{};
                if (SUCCEEDED(adapter->GetDesc1(&adapter_desc))) {
                    result = std::memcmp(&adapter_desc.AdapterLuid, luid, 8) == 0 ? Result::Same
                                                                                 : Result::Other;
                }
                break;
            }
        }
        adapter->Release();
    }
    factory->Release();
    return result;
#else
    (void)hwnd;
    (void)luid;
    return Result::Unknown;
#endif
}

} // namespace Gow3DisplayAdapter
