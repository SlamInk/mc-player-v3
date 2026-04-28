/*
 * Codec Bridge — 协议无关解码抽象层（ADD §5.6.1）。
 *
 * 统辖 MFT 硬解（主路径）+ mc-libcodec 软解（兜底）。
 *   - 能力探测：硬件支持 codec/profile → MFT
 *   - HEVC Extension 缺失 → mc-libcodec H.265 软解（ADR-004）
 *   - 运行期 device_removed/TDR/profile_unsupported → 走 §5.6.5 状态机
 *
 * 输出 VideoFrame 给 FrameValidityGate（dual-bind path 在 emit 前等 ID3D11Fence；§4.3）。
 */

#ifndef MC_PLAYER_MEDIA_CODEC_BRIDGE_H_
#define MC_PLAYER_MEDIA_CODEC_BRIDGE_H_

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include "media/depack_h264.h"
#include "media/depack_h265.h"
#include "media/frame.h"
#include "mc-player/mc_player_types.h"

namespace mcp::media {

class CodecBridge {
public:
    using EmitFn = std::function<void(VideoFrame&&)>;

    CodecBridge(mc_video_codec_t codec,
                Microsoft::WRL::ComPtr<ID3D11Device> device,
                bool allow_software_fallback,
                EmitFn emit);
    ~CodecBridge();

    CodecBridge(const CodecBridge&)            = delete;
    CodecBridge& operator=(const CodecBridge&) = delete;

    /// 提交一个 H.264 AU。
    void submit_h264(H264AccessUnit&& au) noexcept;

    /// 提交一个 H.265 AU。
    void submit_h265(H265AccessUnit&& au) noexcept;

    /// flush（重连 / device-lost 全恢复时）；保留 SPS/PPS。
    void flush() noexcept;

    /// 当前激活的解码器种类（stats 暴露）。
    [[nodiscard]] mc_decoder_kind_t active_kind() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_CODEC_BRIDGE_H_
