/*
 * Codec libcodec — mc-libcodec 软解适配器（ADR-004 / ADR-006 兜底路径）。
 *
 * 当 MFT 路径不可用（如 Win10/11 LTSC 未装 HEVC Extension），Controller 改走本类。
 *
 * 接口与 CodecMftVideo 对齐：submit Annex-B AU → emit VideoFrame。
 * 软解输出 NV12 在 RAM 中，由本类把 RAM → ID3D11Texture2D 上传到一个动态纹理上让
 * RenderD3d11 在同一 device 上采样（避免改 RenderD3d11 的下游接口）。
 */

#ifndef MC_PLAYER_MEDIA_CODEC_LIBCODEC_H_
#define MC_PLAYER_MEDIA_CODEC_LIBCODEC_H_

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include "media/frame.h"
#include "mc-player/mc_player_types.h"

namespace mcp::media {

class CodecLibcodecVideo {
public:
    using EmitFn = std::function<void(VideoFrame&&)>;

    struct Config {
        mc_video_codec_t                       codec = MC_VIDEO_CODEC_H265;
        Microsoft::WRL::ComPtr<ID3D11Device>   device;
        EmitFn                                 emit;
    };

    explicit CodecLibcodecVideo(Config cfg);
    ~CodecLibcodecVideo();

    CodecLibcodecVideo(const CodecLibcodecVideo&)            = delete;
    CodecLibcodecVideo& operator=(const CodecLibcodecVideo&) = delete;

    mc_status_t start() noexcept;
    void        submit(std::span<const uint8_t> au, int64_t pts_us) noexcept;
    void        flush() noexcept;
    void        stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_CODEC_LIBCODEC_H_
