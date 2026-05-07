/*
 * Codec DXVA Video — ID3D11VideoDevice 直驱 HEVC（不经 MFT 桥接层）。
 *
 * 设计动机（ADR-004 兜底场景的快速通路）：
 *   Windows LTSC / 部分 SKU 默认无 HEVC Extension → MFTEnumEx 返 0；但 GPU 驱动通常
 *   仍提供 D3D11VA HEVC profile 的 DDI（CheckVideoDecoderFormat == TRUE）。本类直接
 *   走 ID3D11VideoDevice + DXVA_PicParams_HEVC + DXVA_Slice_HEVCShort，把 MFT 层绕开。
 *
 *   优先级：MFT（首选）→ DXVA-direct（本类，硬件加速）→ libcodec（纯软解，工作量最大）。
 *
 * 复用：
 *   - libcodec 的 SPS/PPS/SliceHeader parser（subprojects/mc-libcodec/src/hevc_parser.h）
 *     直接用于把语法元素填进 DXVA picparams
 *   - 与 CodecMftVideo 一致的 emit 回调与 D3D11 device 共享模型
 */

#ifndef MC_PLAYER_MEDIA_CODEC_DXVA_VIDEO_H_
#define MC_PLAYER_MEDIA_CODEC_DXVA_VIDEO_H_

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "media/frame.h"
#include "mc-player/mc_player_types.h"

namespace mcp::media {

class CodecDxvaVideo {
public:
    using EmitFn = std::function<void(VideoFrame&&)>;

    struct Config {
        mc_video_codec_t                       codec  = MC_VIDEO_CODEC_H265;
        Microsoft::WRL::ComPtr<ID3D11Device>   device;
        EmitFn                                 emit;
    };

    explicit CodecDxvaVideo(Config cfg);
    ~CodecDxvaVideo();

    CodecDxvaVideo(const CodecDxvaVideo&)            = delete;
    CodecDxvaVideo& operator=(const CodecDxvaVideo&) = delete;

    /// 启动；探测 HEVC profile + decoder config + 建 DPB pool；启动 T4-DXVA worker 线程。
    mc_status_t start() noexcept;
    /// 入异步队列；接 vector ownership 避免中间拷贝。
    /// arrival_qpc_ns 是 AU first-packet RX 戳，本类透传到 VideoFrame 用于端到端延时统计。
    void        submit(std::vector<uint8_t> au_bytes, int64_t pts_us,
                        int64_t arrival_qpc_ns = 0) noexcept;
    void        flush() noexcept;
    void        stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// 真实端到端 self-test：跑一帧 64x64 baseline H.264 IDR fixture 走 ID3D11VideoDevice，
/// 读回 NV12 Y plane 校验解码结果合理（≈16 全黑）。
///
/// 用途：解决 Intel UHD 730 / Win11 IoT LTSC 等驱动 silent fail 场景——
/// CheckVideoDecoderFormat 返 TRUE，但 SubmitDecoderBuffers 后 DPB 为全 0 或全 128，
/// 上层无法仅靠表面 supported flag 判断真实能力。
///
/// 用 ADR-015 四档链选档前的"capability-then-select"决策依据。
[[nodiscard]] bool probe_dxva_h264_capable(ID3D11Device* device) noexcept;

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_CODEC_DXVA_VIDEO_H_
