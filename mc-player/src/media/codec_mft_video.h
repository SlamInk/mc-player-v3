/*
 * Codec MFT Video — Windows Media Foundation 异步硬解（ADR-002 / ADD §5.6）。
 *
 * 强约束（ADR-002）：
 *   硬件 MFT 永远 async。Microsoft Learn "Hardware MFTs"：
 *   "All hardware-based MFTs are required to be asynchronous MFTs"。
 *   sync 调用模式不被支持，会 stall。激活时必须显式 unlock async + 用事件生成器
 *   驱动 NeedInput / HaveOutput / DrainComplete 事件循环。
 *
 * 强约束（ADR-003）：
 *   Output 流 BindFlags 显式请求 BIND_DECODER | BIND_SHADER_RESOURCE；
 *   array slice evict 前用 ID3D11Fence (D3D11.3+) 等 SRV 读完，否则花屏。
 *
 * 强约束（§5.6.4）：
 *   B-Frame Policy：检测到 reorder > 0 必须取消 CODECAPI_AVLowLatencyMode，
 *   否则与 low-latency 模式冲突 → 必然花屏。
 *
 * 强约束（§5.6.3）：
 *   CODECAPI_AVLowLatencyMode 类型陷阱 — H.264 decoder 用 VT_UI4，
 *   其他 codec 按惯例用 VT_BOOL，但未在 MS 文档明示，SetValue 前应实测验证。
 */

#ifndef MC_PLAYER_MEDIA_CODEC_MFT_VIDEO_H_
#define MC_PLAYER_MEDIA_CODEC_MFT_VIDEO_H_

#include <Windows.h>
#include <d3d11.h>
#include <mfobjects.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include "media/frame.h"
#include "mc-player/mc_player_types.h"

namespace mcp::media {

class CodecMftVideo {
public:
    using EmitFn = std::function<void(VideoFrame&&)>;

    struct Config {
        mc_video_codec_t                       codec        = MC_VIDEO_CODEC_H264;
        Microsoft::WRL::ComPtr<ID3D11Device>   device;
        bool                                   prefer_low_latency = true;
        uint32_t                               surface_pool_max   = 8;
        EmitFn                                 emit;
    };

    explicit CodecMftVideo(Config cfg);
    ~CodecMftVideo();

    CodecMftVideo(const CodecMftVideo&)            = delete;
    CodecMftVideo& operator=(const CodecMftVideo&) = delete;

    /// 注入 SPS/PPS（H.264）或 VPS/SPS/PPS（H.265）作 extradata，启动 MFT。
    mc_status_t start(std::span<const uint8_t> extradata) noexcept;

    /// 推一个 access unit 进入异步 input 队列；MFT NeedInput 事件触发时拉取。
    void submit(std::span<const uint8_t> au, int64_t pts_us) noexcept;

    void flush() noexcept;
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_CODEC_MFT_VIDEO_H_
