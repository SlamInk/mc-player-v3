/*
 * 视频自适应 Jitter Buffer — 二维 Kalman frame_delay_variation 估计（ADD §5.3.1）。
 *
 * 状态 x = [1/channel_rate, queuing_delay_offset]ᵀ
 * 观测 H = [delta_frame_bytes, 1]
 * 观测 z = observed_frame_delay_variation
 * Q = diag(2.5e-10, 1e-10)
 * R 自适应（突发 vs 稳态）
 * target_delay = max(jitter_estimate, min_target) + max_decode_time
 *
 * 入：RTP datagram；出：按帧重组（按 marker bit）+ NACK 列表（移交 NackModule）。
 * v1 骨架：接口完整，Kalman 估计与 NACK 调度待 Chromium 主线对齐填充。
 */

#ifndef MC_PLAYER_MEDIA_JITTER_BUFFER_VIDEO_H_
#define MC_PLAYER_MEDIA_JITTER_BUFFER_VIDEO_H_

#include <cstdint>
#include <functional>
#include <span>

#include "transport/transport_session.h"

namespace mcp::media {

struct JitterFrame {
    int64_t              pts_us         = 0;
    std::vector<uint8_t> rtp_payload;
    bool                 marker         = false;
    uint16_t             rtp_seq        = 0;
    bool                 has_seq_gap    = false;     // 进入此帧时检测到 gap
};

class JitterBufferVideo {
public:
    using EmitFn   = std::function<void(JitterFrame&&)>;
    using NackFn   = std::function<void(uint16_t pid, uint16_t blp)>;

    JitterBufferVideo(EmitFn emit, NackFn nack) noexcept;

    /// 配置上下界。0 = 用库默认。
    void configure(uint32_t min_target_delay_ms, uint32_t max_target_delay_ms) noexcept;

    void on_rtp(const transport::RtpDatagram& dg) noexcept;
    void reset() noexcept;

    [[nodiscard]] uint32_t target_delay_ms() const noexcept { return target_delay_ms_; }
    [[nodiscard]] uint32_t jitter_estimate_ms() const noexcept { return jitter_estimate_ms_; }

private:
    EmitFn   emit_;
    NackFn   nack_;
    uint32_t min_target_ms_      = 0;
    uint32_t max_target_ms_      = 0;
    uint32_t target_delay_ms_    = 0;
    uint32_t jitter_estimate_ms_ = 0;
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_JITTER_BUFFER_VIDEO_H_
