#include "media/jitter_buffer_video.h"

#include "pal/log.h"
#include "pal/metric.h"
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

void JitterBufferVideo::set_mode(Mode m) noexcept {
    mode_ = m;
    // Phase 9.3 主线: ZeroJitter 时 target_delay clamp 到 0~1ms,Kalman 估计停摆;
    //   双缓冲实现切换不丢帧(reload 时新 mode 后台加热 → swap → 旧 buffer 排空 GC)。
    // 当前结构性: 仅落 mode_ 字段 + metric。
    if (m == Mode::ZeroJitter) {
        target_delay_ms_ = 1;     // SDI_REPLACEMENT 目标
    }
    pal::metric::Registry::instance().gauge("mc.jitter.mode")
        .set(static_cast<int64_t>(m));
    pal::metric::Registry::instance().gauge("mc.jitter.target_delay_ms")
        .set(static_cast<int64_t>(target_delay_ms_));
    MCP_LOGF(pal::LogLevel::info,
             "JitterBufferVideo: mode=%d target_delay=%ums",
             static_cast<int>(m), target_delay_ms_);
}

}  // namespace mcp::media
