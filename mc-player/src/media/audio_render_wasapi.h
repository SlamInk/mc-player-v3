/*
 * WASAPI 共享模式音频渲染（ADD §5.11）。
 *
 * - IAudioClient3 事件驱动；buffer period 取 GetSharedModeEnginePeriod 的 driver-reported
 *   pMinPeriodInFrames（HDAudio 典型 128 frames @ 48 kHz ≈ 2.66 ms；default engine ≈ 10 ms）。
 * - AutoConvert PCM + default-quality SRC，由 WASAPI 完成设备 mix 转换。
 * - IMMNotificationClient::OnDefaultDeviceChanged 拔耳机 → tear down → 新 device 重建 → 复位 jitter。
 */

#ifndef MC_PLAYER_MEDIA_AUDIO_RENDER_WASAPI_H_
#define MC_PLAYER_MEDIA_AUDIO_RENDER_WASAPI_H_

#include <cstdint>
#include <memory>

#include "media/frame.h"
#include "mc-player/mc_player_types.h"

namespace mcp::media {

class AudioRenderWasapi {
public:
    AudioRenderWasapi();
    ~AudioRenderWasapi();

    AudioRenderWasapi(const AudioRenderWasapi&)            = delete;
    AudioRenderWasapi& operator=(const AudioRenderWasapi&) = delete;

    mc_status_t start(uint32_t sample_rate_hz, uint32_t channels) noexcept;
    void stop() noexcept;

    /// 提交一帧 PCM。线程安全；内部进 ring buffer。
    void submit(AudioFrame&& frame) noexcept;

    /// 主时钟读数（us）— A/V 同步用。
    [[nodiscard]] int64_t master_clock_us() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_AUDIO_RENDER_WASAPI_H_
