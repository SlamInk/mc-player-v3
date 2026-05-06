#include "preset/preset_definitions.h"

namespace mcp::preset {

namespace {

const Preset kSdiReplacement = {
    MC_PRESET_SDI_REPLACEMENT, "SDI_REPLACEMENT",
    JitterMode::ZeroJitter,           1,                   // 9.3 unlock
    RtcpMode::ReducedSize,                                  // 9.2 unlock
    PresentMode::RaceToDisplay,                             // 9.4 unlock
    MC_RENDER_PROFILE_ULTIMATE_DCOMP,
    /*gate_strict_color=*/false,                            // 9.1 NV12 直显路径不参与
    /*gate_strict_fence=*/false,
};

const Preset kRealtimeLan = {
    MC_PRESET_REALTIME_LAN, "REALTIME_LAN",
    JitterMode::Kalman_Aggressive,    3,
    RtcpMode::AvpfImmediate,
    PresentMode::AllowTearing,
    MC_RENDER_PROFILE_EXTREME,
    /*gate_strict_color=*/true,
    /*gate_strict_fence=*/true,
};

const Preset kStreamingWifi = {
    MC_PRESET_STREAMING_WIFI, "STREAMING_WIFI",
    JitterMode::Kalman_Adaptive,      30,
    RtcpMode::AvpfImmediate,
    PresentMode::VsyncAligned,
    MC_RENDER_PROFILE_BALANCED,
    /*gate_strict_color=*/true,
    /*gate_strict_fence=*/true,
};

const Preset kWanFallback = {
    MC_PRESET_WAN_FALLBACK, "WAN_FALLBACK",
    JitterMode::Kalman_Adaptive,      80,
    RtcpMode::Regular,
    PresentMode::VsyncAligned,
    MC_RENDER_PROFILE_COMPAT,
    /*gate_strict_color=*/true,
    /*gate_strict_fence=*/true,
};

const Preset kSafeMode = {
    MC_PRESET_SAFE_MODE, "SAFE_MODE",
    JitterMode::Kalman_Adaptive,      150,
    RtcpMode::Regular,
    PresentMode::VsyncAligned,
    MC_RENDER_PROFILE_COMPAT,
    /*gate_strict_color=*/true,
    /*gate_strict_fence=*/true,
};

const Preset kNone = { MC_PRESET_NONE, "NONE",
                        JitterMode::Kalman_Adaptive, 5,
                        RtcpMode::Regular, PresentMode::VsyncAligned,
                        MC_RENDER_PROFILE_AUTO,
                        true, true };

}  // namespace

const Preset& preset_definition(mc_preset_id_t id) noexcept {
    switch (id) {
        case MC_PRESET_SDI_REPLACEMENT: return kSdiReplacement;
        case MC_PRESET_REALTIME_LAN:    return kRealtimeLan;
        case MC_PRESET_STREAMING_WIFI:  return kStreamingWifi;
        case MC_PRESET_WAN_FALLBACK:    return kWanFallback;
        case MC_PRESET_SAFE_MODE:       return kSafeMode;
        default:                         return kNone;
    }
}

}  // namespace mcp::preset
