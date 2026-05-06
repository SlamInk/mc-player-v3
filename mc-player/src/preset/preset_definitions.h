/*
 * Preset Definitions — 5 档 preset 参数表(ADR-017 / capability_probe §6.1)。
 *
 * 5 档(优先级降序):
 *   1. SDI_REPLACEMENT — LAN-switched + NVDEC + 240Hz VRR + zerolatency 编码;
 *      端到端 ≤8ms 目标(子目标 1-4 全命中)
 *   2. REALTIME_LAN    — LAN + 任 vendor SDK / DXVA + ≥120Hz;端到端 ≤20ms
 *   3. STREAMING_WIFI  — Wi-Fi + 任 codec;端到端 ≤80ms,容忍轻度 jitter
 *   4. WAN_FALLBACK    — WAN 公网 + 高 loss / 高 RTT;端到端 ≤200ms,激进 jitter buffer
 *   5. SAFE_MODE       — apply 整体回滚兜底(永远可用,功能阉割)
 */

#ifndef MC_PLAYER_PRESET_DEFINITIONS_H_
#define MC_PLAYER_PRESET_DEFINITIONS_H_

#include <cstdint>

#include "mc-player/mc_player_stats.h"
#include "mc-player/mc_player_types.h"

namespace mcp::preset {

enum class JitterMode : uint8_t {
    Kalman_Aggressive = 0,
    Kalman_Adaptive   = 1,
    ZeroJitter        = 2,
};

enum class RtcpMode : uint8_t {
    Regular         = 0,    // 标准 RTCP RR/SR + RR
    AvpfImmediate   = 1,    // RFC 4585 trr-int=0
    ReducedSize     = 2,    // RFC 5506 + AVPF Immediate
};

enum class PresentMode : uint8_t {
    VsyncAligned    = 0,
    AllowTearing    = 1,
    RaceToDisplay   = 2,    // Reflex 风格,VRR 必需
};

struct Preset {
    mc_preset_id_t      id              = MC_PRESET_NONE;
    const char*         name            = "none";
    JitterMode          jitter          = JitterMode::Kalman_Aggressive;
    uint32_t            jitter_target_delay_ms = 5;
    RtcpMode            rtcp            = RtcpMode::Regular;
    PresentMode         present         = PresentMode::VsyncAligned;
    mc_render_profile_t render_profile  = MC_RENDER_PROFILE_BALANCED;
    bool                gate_strict_color = true;     // SDI_REPLACEMENT NV12 直显时改 false
    bool                gate_strict_fence = true;
};

/// 取 preset 表(编译期常量;capability_probe §6.1 1:1 对应)。
const Preset& preset_definition(mc_preset_id_t id) noexcept;

}  // namespace mcp::preset

#endif  // MC_PLAYER_PRESET_DEFINITIONS_H_
