/*
 * NVDEC — NVIDIA Video Codec SDK 直驱档 1 实装（ADR-015 §5.6.2.1）。
 *
 * 实装边界（Phase 5）：
 *   本 commit 落地"结构性骨架 + 动态加载 + vendor 探测 + skip reason 分类"。
 *   开发机 Intel UHD 730 无 NV dGPU,无法运行验 decode 路径;实际 cuvid* 解码循环
 *   (parser callback / decode picture / map / CUDA-D3D11 interop) 留给 NV 硬件
 *   可达的 Phase 5b commit 收尾。
 *
 *   Phase 5 现可验路径：
 *     - VendorId == 0x10DE (NVIDIA) 检测
 *     - LoadLibraryW("nvcuvid.dll") 探测
 *     - 13 个关键 cuvid 入口 GetProcAddress
 *     - 失败时 record_skip 分类 (vendor_mismatch / sdk_missing / sdk_init_failed)
 *
 *   Phase 5b 待补：
 *     - cuvidCreateVideoParser + ulMaxDisplayDelay=0 + 回调 dispatch
 *     - cuvidDecodePicture + cuvidMapVideoFrame + cudaGraphicsD3D11RegisterResource
 *     - D3D11 fence wait 与 NV12 array slice 同步
 *
 * 不依赖 NVIDIA Video Codec SDK 头：cuvid* 入口签名按 NVIDIA 公开文档自描述
 * (typedef 在 .cpp 内私有);只用 driver API (cu* / cuvid*),不用 cudart toolkit
 * (规避版本耦合,与 Phase 5 §5.4 已知风险对应)。
 */

#ifndef MC_PLAYER_MEDIA_CODEC_NVDEC_H_
#define MC_PLAYER_MEDIA_CODEC_NVDEC_H_

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "media/frame.h"
#include "mc-player/mc_player_types.h"

namespace mcp::media {

class CodecNvdec {
public:
    using EmitFn = std::function<void(VideoFrame&&)>;

    struct Config {
        mc_video_codec_t                       codec   = MC_VIDEO_CODEC_H264;
        Microsoft::WRL::ComPtr<ID3D11Device>   device;
        EmitFn                                 emit;
    };

    explicit CodecNvdec(Config cfg);
    ~CodecNvdec();

    CodecNvdec(const CodecNvdec&)            = delete;
    CodecNvdec& operator=(const CodecNvdec&) = delete;

    /// 启动；按以下顺序尝试,任一失败立即返回对应 status:
    ///   1. 检查 cfg.device → 取 IDXGIAdapter VendorId
    ///   2. VendorId != 0x10DE → MC_ERR_UNSUPPORTED (vendor_mismatch)
    ///   3. LoadLibraryW("nvcuvid.dll") → MC_ERR_NO_HARDWARE (sdk_missing)
    ///   4. GetProcAddress 13 个 cuvid 入口 → MC_ERR_INTERNAL (sdk_init_failed)
    ///   5. Phase 5b: 创建 parser / decoder / CUDA context → 实装中,当前 stub 返
    ///      MC_ERR_UNSUPPORTED (sdk_decode_pending) 让 controller 降到档 2
    mc_status_t start() noexcept;

    void        submit(std::vector<uint8_t> au_bytes, int64_t pts_us,
                        int64_t arrival_qpc_ns = 0) noexcept;
    void        flush() noexcept;
    void        stop() noexcept;

    /// 探测原因（仅 start() 失败时有效）。controller 用以填 mc.probe.tier_skip_reason。
    enum class StartReason : uint8_t {
        ok                  = 0,
        vendor_mismatch     = 1,
        sdk_missing         = 2,
        sdk_init_failed     = 3,
        sdk_decode_pending  = 4,
        profile_unsupported = 5,
        device_invalid      = 6,
    };
    [[nodiscard]] StartReason last_start_reason() const noexcept;

    /// 静态探测：仅查 VendorId + nvcuvid.dll 存在性（不打开 device）。
    /// 用于 caps_probe / 启动前的轻量检查。
    [[nodiscard]] static bool probe_sdk_dll_present() noexcept;
    [[nodiscard]] static const char* reason_label(StartReason r) noexcept;

    // Phase 9.5 子目标 5:CUDA Graphs 探索(NVDEC 路径专属,可选)。
    //   把 alloc + parse + decode + map 打包成单次 CUDA Graph 提交,预期 -1~2ms。
    //   失败 fallback 到原 streaming 提交;不影响 SDI_REPLACEMENT 命中(探索性)。
    void enable_cuda_graphs(bool on) noexcept;
    [[nodiscard]] bool cuda_graphs_active() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_CODEC_NVDEC_H_
