/*
 * 音频自适应 Jitter Buffer — iat 直方图 + NetEQ 三相位（ADD §5.3.2）。
 *
 * 三相位（参考 Chromium NetEQ delay_manager.cc）：
 *   - buffer > target+30ms → Accelerate（WSOLA 压缩 5–10%）
 *   - buffer < target−10ms → Preemptive Expand（拉伸）
 *   - 丢包 → Normal Expand（PLC）；G.711 简化为重放上个包
 *
 * 时钟跳变检测（NTP 校时引发）：相邻 RTP 时间戳差超阈值 → reset 估计。
 * v1 骨架：接口完整，NetEQ 估计填充留迭代。
 */

#ifndef MC_PLAYER_MEDIA_JITTER_BUFFER_AUDIO_H_
#define MC_PLAYER_MEDIA_JITTER_BUFFER_AUDIO_H_

#include <cstdint>
#include <functional>

#include "transport/transport_session.h"

namespace mcp::media {

struct AudioJitterFrame {
    int64_t              pts_us = 0;
    std::vector<uint8_t> rtp_payload;
    uint16_t             rtp_seq = 0;
};

class JitterBufferAudio {
public:
    using EmitFn = std::function<void(AudioJitterFrame&&)>;
    explicit JitterBufferAudio(EmitFn emit) noexcept;

    void on_rtp(const transport::RtpDatagram& dg) noexcept;
    void reset() noexcept;

    [[nodiscard]] uint32_t buffer_ms() const noexcept { return buffer_ms_; }

private:
    EmitFn   emit_;
    uint32_t buffer_ms_ = 0;
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_JITTER_BUFFER_AUDIO_H_
