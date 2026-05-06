/*
 * Capability Snapshot — 四维 probe 聚合(ADR-017 / capability_probe §3)。
 *
 * 输入:hardware ∩ network ∩ encoder ∩ render 四个独立 probe 的结果;
 * 输出:统一 CapabilitySnapshot 给 PresetSelector 消费。
 *
 * 实装边界(Phase 9.0 结构性骨架):
 *   - 四个子 snapshot 类已定义但默认值保守(SAFE_MODE 友好)
 *   - 完整 probe 实装在 9.0 主线 commit 后再批量补
 */

#ifndef MC_PLAYER_PROBE_CAPABILITY_SNAPSHOT_H_
#define MC_PLAYER_PROBE_CAPABILITY_SNAPSHOT_H_

#include <cstdint>
#include <string>

#include "mc-player/mc_player_stats.h"
#include "mc-player/mc_player_types.h"

namespace mcp::probe {

// ─── Hardware (ADR-017 / capability_probe §3.2) ───────────────────
struct HardwareSnapshot {
    uint32_t  vendor_id              = 0;
    bool      is_software_adapter    = true;
    bool      h264_supported         = false;
    bool      hevc_main_supported    = false;
    bool      hevc_main10_supported  = false;
    bool      av1_supported          = false;
    bool      dual_bind_supported    = false;
    bool      tearing_supported      = false;
    bool      vendor_sdk_present     = false;     // 任一 NVDEC/oneVPL/AMF DLL 在场
    bool      hevc_extension_present = false;     // Microsoft HEVC Extension package
    bool      complete               = false;     // probe 跑完才 true
};

// ─── Network (ADR-018 / capability_probe §3.3) ────────────────────
struct NetworkSnapshot {
    uint32_t       rtt_p50_ms        = 0;
    uint32_t       rtt_p95_ms        = 0;
    uint32_t       rtt_p99_ms        = 0;
    double         loss_rate_short   = 0.0;     // 5s 滑窗 loss 率
    double         iat_jitter_p95_ms = 0.0;     // first GOP 内 RTP iat jitter
    mc_link_kind_t link_kind         = MC_LINK_KIND_UNKNOWN;
    bool           complete          = false;
};

// ─── Encoder (ADR-019 / capability_probe §3.4) ────────────────────
struct EncoderSnapshot {
    uint32_t reorder_depth        = 0;     // 0=zerolatency / >0=有 B 帧
    bool     low_latency_signaled = false; // SDP profile-level-id 提示
    bool     bitstream_restriction_known = false;     // SPS VUI 给了 max_num_reorder_frames
    uint32_t first_gop_dts_pts_diff_ms = 0;            // 首 GOP 实测兜底
    bool     complete             = false;
};

// ─── Render (capability_probe §3.5) ──────────────────────────────
struct RenderSnapshot {
    uint32_t refresh_rate_hz             = 60;
    bool     vrr_supported               = false;
    bool     dcomp_supported             = false;
    uint32_t mpo_planes                  = 0;
    bool     hardware_composition_supported = false;
    bool     supports_dcomp_nv12_direct  = false;     // Phase 9.1 才设 true
    bool     complete                    = false;
};

struct CapabilitySnapshot {
    HardwareSnapshot hw;
    NetworkSnapshot  net;
    EncoderSnapshot  enc;
    RenderSnapshot   render;

    [[nodiscard]] bool complete() const noexcept {
        return hw.complete && net.complete && enc.complete && render.complete;
    }
};

}  // namespace mcp::probe

#endif  // MC_PLAYER_PROBE_CAPABILITY_SNAPSHOT_H_
