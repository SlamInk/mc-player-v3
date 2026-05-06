#include "probe/encoder_probe.h"

#include <atomic>

namespace mcp::probe {

struct EncoderProbe::Impl {
    std::atomic<uint32_t> reorder_depth{0};
    std::atomic<bool>     low_latency_signaled{false};
    std::atomic<bool>     bitstream_restriction_known{false};
    std::atomic<uint32_t> first_gop_dts_pts_diff_ms{0};
    std::atomic<bool>     first_gop_seen{false};
};

EncoderProbe::EncoderProbe() : impl_{std::make_unique<Impl>()} {}
EncoderProbe::~EncoderProbe() = default;

void EncoderProbe::record_sdp_profile_level_id(std::string_view /*profile_level_id*/) noexcept {
    // Phase 9.0 主线: 解析 H.264 profile-level-id 6 hex digit (profile_idc/constraint/level_idc)。
    //   profile_idc=66 (Baseline) / 77 (Main) → 通常无 B 帧;
    //   profile_idc=100 (High) → 可能有 B 帧,需配合 SPS VUI 确认。
    impl_->low_latency_signaled.store(false, std::memory_order_relaxed);
}

void EncoderProbe::record_sps_vui(uint32_t max_num_reorder_frames,
                                    bool bitstream_restriction_known) noexcept {
    impl_->reorder_depth.store(max_num_reorder_frames, std::memory_order_relaxed);
    impl_->bitstream_restriction_known.store(bitstream_restriction_known,
                                              std::memory_order_relaxed);
}

void EncoderProbe::record_first_gop_dts_pts_diff_ms(uint32_t diff_ms) noexcept {
    impl_->first_gop_dts_pts_diff_ms.store(diff_ms, std::memory_order_relaxed);
    impl_->first_gop_seen.store(true, std::memory_order_relaxed);
}

EncoderSnapshot EncoderProbe::snapshot() const noexcept {
    EncoderSnapshot out;
    out.reorder_depth                = impl_->reorder_depth.load(std::memory_order_relaxed);
    out.low_latency_signaled         = impl_->low_latency_signaled.load(std::memory_order_relaxed);
    out.bitstream_restriction_known  = impl_->bitstream_restriction_known.load(std::memory_order_relaxed);
    out.first_gop_dts_pts_diff_ms    = impl_->first_gop_dts_pts_diff_ms.load(std::memory_order_relaxed);
    out.complete                     = impl_->first_gop_seen.load(std::memory_order_relaxed);
    return out;
}

bool EncoderProbe::ready() const noexcept {
    return impl_->first_gop_seen.load(std::memory_order_relaxed);
}

}  // namespace mcp::probe
