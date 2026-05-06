#include "probe/render_probe.h"

#include <dxgi1_6.h>

#include "pal/log.h"
#include "pal/metric.h"

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
    out.supports_dcomp_nv12_direct  = false;    // Phase 9.1 unlock
    out.complete                    = true;

    pal::metric::Registry::instance().gauge("mc.render.display_refresh_hz")
        .set(static_cast<int64_t>(out.refresh_rate_hz));
    return out;
}

}  // namespace mcp::probe
