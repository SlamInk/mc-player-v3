#include "media/av_sync.h"

#include <vector>

namespace mcp::media {

namespace {
constexpr std::size_t kRegressionWindow = 16;
}

AvSync::AvSync(AudioClockFn clock) noexcept : audio_clock_{std::move(clock)} {}

void AvSync::on_rtcp_sr(const NtpRtpSample& s) noexcept {
    samples_.push_back(s);
    if (samples_.size() > kRegressionWindow) {
        samples_.erase(samples_.begin());
    }
    // TODO: 滑窗最小二乘 ntp = a·rtp_ts + b；更新 a_/b_。
}

int64_t AvSync::schedule_render_us(uint32_t /*rtp_timestamp*/, uint32_t /*clock_rate_hz*/) const noexcept {
    // TODO: 把 rtp_timestamp 经回归映射到 NTP us，再映射到音频主时钟同步面。
    return audio_clock_ ? audio_clock_() : 0;
}

}  // namespace mcp::media
