#include "media/jitter_buffer_video.h"

#include "transport/rtp_packet.h"

namespace mcp::media {

JitterBufferVideo::JitterBufferVideo(EmitFn emit, NackFn nack) noexcept
    : emit_{std::move(emit)}, nack_{std::move(nack)} {}

void JitterBufferVideo::configure(uint32_t min_target_delay_ms,
                                   uint32_t max_target_delay_ms) noexcept {
    min_target_ms_   = min_target_delay_ms;
    max_target_ms_   = max_target_delay_ms;
    target_delay_ms_ = std::max(min_target_ms_, target_delay_ms_);
}

void JitterBufferVideo::on_rtp(const transport::RtpDatagram& dg) noexcept {
    transport::RtpPacket pkt;
    if (!transport::parse_rtp(dg.bytes, &pkt)) return;

    JitterFrame f;
    f.pts_us       = dg.arrival_qpc_ns / 1000;       // 占位：v1 用到达时间近似
    f.rtp_payload.assign(pkt.payload.begin(), pkt.payload.end());
    f.marker       = pkt.marker;
    f.rtp_seq      = pkt.sequence;
    if (emit_) emit_(std::move(f));

    // TODO: 完整 Kalman 估计 / target_delay 调度 / NACK 触发 — 对齐 Chromium 主线。
}

void JitterBufferVideo::reset() noexcept {
    target_delay_ms_    = min_target_ms_;
    jitter_estimate_ms_ = 0;
}

}  // namespace mcp::media
