#include "preset/preset_selector.h"

#include "pal/log.h"
#include "pal/metric.h"

namespace mcp::preset {

namespace {

bool match_sdi(const probe::CapabilitySnapshot& s) noexcept {
    return s.hw.vendor_sdk_present
        && s.net.link_kind == MC_LINK_KIND_LAN_SWITCHED
        && s.net.loss_rate_short < 0.001
        && s.net.rtt_p95_ms < 5
        && s.enc.reorder_depth == 0
        && s.render.refresh_rate_hz >= 144
        && s.render.vrr_supported
        && s.render.supports_dcomp_nv12_direct;
}

bool match_realtime_lan(const probe::CapabilitySnapshot& s) noexcept {
    return (s.hw.h264_supported || s.hw.hevc_main_supported)
        && (s.net.link_kind == MC_LINK_KIND_LAN_SWITCHED
            || s.net.link_kind == MC_LINK_KIND_LAN_WIFI)
        && s.net.rtt_p95_ms < 20
        && s.net.loss_rate_short < 0.01;
}

bool match_streaming_wifi(const probe::CapabilitySnapshot& s) noexcept {
    return s.net.link_kind == MC_LINK_KIND_LAN_WIFI
        && s.net.loss_rate_short < 0.03;
}

bool match_wan_fallback(const probe::CapabilitySnapshot& s) noexcept {
    return s.net.loss_rate_short >= 0.03 || s.net.rtt_p95_ms >= 100;
}

}  // namespace

mc_preset_id_t select_preset(const probe::CapabilitySnapshot& snap) noexcept {
    if (!snap.complete()) {
        // probe 未完整 → 启动期保守用 REALTIME_LAN(BOOTSTRAP 默认),
        // probe 完成后 controller 触发 reload 切到正式 preset。
        return MC_PRESET_REALTIME_LAN;
    }

    mc_preset_id_t selected = MC_PRESET_SAFE_MODE;
    if (match_sdi(snap))                  selected = MC_PRESET_SDI_REPLACEMENT;
    else if (match_realtime_lan(snap))    selected = MC_PRESET_REALTIME_LAN;
    else if (match_streaming_wifi(snap))  selected = MC_PRESET_STREAMING_WIFI;
    else if (match_wan_fallback(snap))    selected = MC_PRESET_WAN_FALLBACK;
    else                                   selected = MC_PRESET_SAFE_MODE;

    pal::metric::Registry::instance().gauge("mc.preset.active_id")
        .set(static_cast<int64_t>(selected));
    MCP_LOGF(pal::LogLevel::info,
             "PresetSelector: selected=%d (hw.vendor_sdk=%d hw.h264=%d hw.hevc=%d "
             "net.link=%d net.rtt95=%u net.loss=%.4f enc.reorder=%u "
             "render.hz=%u render.vrr=%d render.nv12_direct=%d)",
             selected, snap.hw.vendor_sdk_present, snap.hw.h264_supported, snap.hw.hevc_main_supported,
             static_cast<int>(snap.net.link_kind), snap.net.rtt_p95_ms, snap.net.loss_rate_short,
             snap.enc.reorder_depth, snap.render.refresh_rate_hz,
             snap.render.vrr_supported, snap.render.supports_dcomp_nv12_direct);
    return selected;
}

}  // namespace mcp::preset
