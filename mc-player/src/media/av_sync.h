/*
 * A/V 同步（ADD §5.9）。
 *
 * 主时钟：WASAPI master clock — IAudioClock::GetPosition 取「已播放样本数」+ 对应 QPC。
 *
 * NTP-RTP 线性回归：
 *   收到 RTCP SR 入采样 <ntp, rtp_ts>；最近 N 个滑动窗口最小二乘拟合 ntp = a · rtp_ts + b。
 *
 * 调度阈值（ITU-R BT.1359 收紧）：
 *   −25 ≤ av_offset ≤ +15 立即渲染
 *   > +15 (视频迟于音频) 丢帧
 *   < −25 (视频 PTS 早于音频) 延迟渲染
 */

#ifndef MC_PLAYER_MEDIA_AV_SYNC_H_
#define MC_PLAYER_MEDIA_AV_SYNC_H_

#include <cstdint>
#include <functional>

namespace mcp::media {

struct NtpRtpSample {
    uint64_t ntp_timestamp;
    uint32_t rtp_timestamp;
};

class AvSync {
public:
    using AudioClockFn = std::function<int64_t()>;     // 返回当前音频主时钟 us

    explicit AvSync(AudioClockFn clock) noexcept;

    void on_rtcp_sr(const NtpRtpSample& s) noexcept;

    /// 给定 frame 的 RTP timestamp，返回它应该在主时钟的哪个 us 渲染。
    int64_t schedule_render_us(uint32_t rtp_timestamp, uint32_t clock_rate_hz) const noexcept;

    /// 当前 audio - video 偏差；正值音频领先。
    [[nodiscard]] int64_t av_offset_us() const noexcept { return av_offset_us_; }

private:
    AudioClockFn         audio_clock_;
    std::vector<NtpRtpSample> samples_;
    double               a_ = 0.0;
    double               b_ = 0.0;
    int64_t              av_offset_us_ = 0;
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_AV_SYNC_H_
