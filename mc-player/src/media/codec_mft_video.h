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
 *   v1 简化路径：MFT 输出 sample 通过 IMFDXGIBuffer 取原生 texture，本类立即
 *   CopySubresourceRegion 到自管 SR-only NV12 array pool；MFT 与 renderer 共用
 *   同一 immediate context（ID3D10Multithread 串行化），CopySub 与后续 SRV 采样
 *   天然按提交顺序串行 → 无 SRV-evict race → 无需显式 ID3D11Fence。
 *   代价：每帧多一次 GPU intra-device blit ~0.1-0.2 ms（属于零拷贝定义内
 *   "GPU 内至多 1 次 NV12 array slice 复制"，符合 ADD §4.3）。
 *   收益：降低出错面，避免 dual-bind BindFlags 在部分 driver（AMD 早期）创建
 *   NV12 SRV 时返回 E_INVALIDARG。日后启用多 deferred context 或拆 T5 独立
 *   render 线程时，需切回 BIND_DECODER | BIND_SHADER_RESOURCE + ID3D11Fence。
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
#include <vector>

#include "media/color_meta.h"
#include "media/frame.h"
#include "mc-player/mc_player_types.h"

namespace mcp::media {

class CodecMftVideo {
public:
    using EmitFn = std::function<void(VideoFrame&&)>;

    struct Config {
        mc_video_codec_t                       codec        = MC_VIDEO_CODEC_H264;
        Microsoft::WRL::ComPtr<ID3D11Device>   device;
        // prefer_low_latency=true 时启动 CODECAPI_AVLowLatencyMode（H.264 VT_UI4=1，
        // 其余 codec 兜底 VT_BOOL=TRUE）。检测到 reorder>0（B 帧）时由 caller 置 false，
        // 否则与 LowLatency 模式直接冲突 → ADD §5.6.4 明示必然花屏。
        bool                                   prefer_low_latency = true;
        uint32_t                               surface_pool_max   = 8;
        // Color VUI 三级兜底（ADD §5.12）的最终值：caller 调用 resolve_color() 计算，
        // 此处仅作为只读输入应用到每个输出 frame。MC_*_AUTO 表示 caller 未设、用 BT.709 默认。
        ResolvedColor                          color {
            MC_COLOR_PRIMARIES_BT709,
            MC_COLOR_RANGE_LIMITED,
            MC_COLOR_MATRIX_BT709,
        };
        EmitFn                                 emit;
    };

    explicit CodecMftVideo(Config cfg);
    ~CodecMftVideo();

    CodecMftVideo(const CodecMftVideo&)            = delete;
    CodecMftVideo& operator=(const CodecMftVideo&) = delete;

    /// 注入 SPS/PPS（H.264）或 VPS/SPS/PPS（H.265）作 extradata，启动 MFT。
    mc_status_t start(std::span<const uint8_t> extradata) noexcept;

    /// 推一个 access unit 进入异步 input 队列；MFT NeedInput 事件触发时拉取。
    /// 接收 vector ownership，避免 controller→codec 链条上的额外 memcpy。
    void submit(std::vector<uint8_t> au_bytes, int64_t pts_us) noexcept;

    void flush() noexcept;
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_CODEC_MFT_VIDEO_H_
