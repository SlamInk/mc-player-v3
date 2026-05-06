#include "probe/render_probe.h"

#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "pal/log.h"
#include "pal/metric.h"

#pragma comment(lib, "dcomp.lib")

namespace mcp::probe {

namespace {

uint32_t query_refresh_rate(HWND hwnd) noexcept {
    if (!hwnd) return 60;
    HMONITOR mon = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!mon) return 60;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(mon, &mi)) return 60;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!::EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 60;
    return dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 60;
}

// Phase 9.1: 试探创建 NV12 composition swapchain。
// 创建临时 D3D11 device + IDXGIFactory2,调 CreateSwapChainForComposition
// 试探 DXGI_FORMAT_NV12;成功表明 driver + DCOMP 链路支持 NV12 直显(MPO 多面合成)。
bool probe_dcomp_nv12_direct() noexcept {
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;

    ComPtr<ID3D11Device> device;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = ::D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &device, &fl, nullptr);
    if (FAILED(hr) || !device) return false;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width            = 256;
    sd.Height           = 256;
    sd.Format           = DXGI_FORMAT_NV12;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount      = 2;
    sd.Scaling          = DXGI_SCALING_STRETCH;
    sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode        = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> sc;
    hr = factory->CreateSwapChainForComposition(device.Get(), &sd, nullptr, &sc);
    return SUCCEEDED(hr);
}

}  // namespace

RenderSnapshot run_render_probe(HWND hwnd) noexcept {
    pal::metric::ScopedTimer t{
        pal::metric::Registry::instance().timer("mc.probe.render_complete_ns")};

    RenderSnapshot out;
    out.refresh_rate_hz = query_refresh_rate(hwnd);

    // tearing 通过 IDXGIFactory6::CheckFeatureSupport 探测:Phase 9.0 主线接现有
    // dxgi_caps_probe 而不重做(已经在 hardware_probe 通过 caps.allow_tearing_supported 暴露)。
    // 这里仅占位 false,以防 hardware_probe 还未跑完。
    out.vrr_supported               = false;
    out.dcomp_supported             = true;     // DCompositionCreateDevice2 在 Win8+ 永远可用
    out.mpo_planes                  = 1;         // 默认 1 plane;实际值需 IDXGIOutput6 查询(Phase 9.1)
    out.hardware_composition_supported = false;  // PresentMon-equivalent ETW 集成(Phase 3 后续)
    out.supports_dcomp_nv12_direct  = probe_dcomp_nv12_direct();
    pal::metric::Registry::instance().gauge("mc.render.dcomp_nv12_direct_active")
        .set(out.supports_dcomp_nv12_direct ? 1 : 0);
    out.complete                    = true;

    pal::metric::Registry::instance().gauge("mc.render.display_refresh_hz")
        .set(static_cast<int64_t>(out.refresh_rate_hz));
    return out;
}

}  // namespace mcp::probe
