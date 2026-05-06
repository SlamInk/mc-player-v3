/*
 * oneVPL — Intel oneVPL / Media SDK 直驱档 1 实装(ADR-015 §5.6.2.1)。
 *
 * 实装边界(Phase 6):
 *   本 commit 落地"结构性骨架 + 动态加载 + vendor 探测 + skip reason 分类"，
 *   镜像 Phase 5a NVDEC 模式。开发机虽是 Intel UHD 730 可达 oneVPL,但 oneVPL
 *   解码循环(MFXLoad / DecodeHeader / DecodeFrameAsync / mfxFrameSurface1
 *   →  D3D11 互通)涉及 ~600 行复杂 SDK ABI,与 Phase 5b/7b 风格统一,留 Phase 6b。
 *
 *   Phase 6 现可验路径：
 *     - VendorId == 0x8086 (Intel) 检测
 *     - LoadLibraryW("vpl.dll") 优先,回退 LoadLibraryW("libmfx.dll")
 *     - 关键 oneVPL 入口 GetProcAddress(MFXLoad / MFXCreateSession /
 *       MFXVideoDECODE_*) — 任一缺失即 sdk_init_failed
 *     - 失败时 record_skip 分类(vendor_mismatch / sdk_missing / sdk_init_failed
 *       / sdk_decode_pending)
 *
 *   Phase 6b 待补：
 *     - MFXLoad + MFXCreateSession(MFX_IMPL_HARDWARE) + MFXVideoCORE_SetHandle
 *       (D3D11 device 共享)
 *     - mfxBitstream + DecodeHeader + DecodeFrameAsync + Sync + mfxFrameSurface1
 *       NV12 直拿 D3D11 texture
 *     - AsyncDepth=1 + MFX_BITSTREAM_COMPLETE_FRAME(性能量度规范 §6 / plan §6.0)
 *
 * 不依赖 oneVPL/MediaSDK 头：函数指针签名 typedef 按 oneVPL 公开文档自描述
 * (typedef 在 .cpp 内私有);driver 端 API,不引 cmake target / 不入 vcpkg。
 */

#ifndef MC_PLAYER_MEDIA_CODEC_ONEVPL_H_
#define MC_PLAYER_MEDIA_CODEC_ONEVPL_H_

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

class CodecOneVPL {
public:
    using EmitFn = std::function<void(VideoFrame&&)>;

    struct Config {
        mc_video_codec_t                       codec   = MC_VIDEO_CODEC_H264;
        Microsoft::WRL::ComPtr<ID3D11Device>   device;
        EmitFn                                 emit;
    };

    explicit CodecOneVPL(Config cfg);
    ~CodecOneVPL();

    CodecOneVPL(const CodecOneVPL&)            = delete;
    CodecOneVPL& operator=(const CodecOneVPL&) = delete;

    /// 启动；按以下顺序尝试,任一失败立即返回对应 status:
    ///   1. 检查 cfg.device → 取 IDXGIAdapter VendorId
    ///   2. VendorId != 0x8086 → MC_ERR_UNSUPPORTED (vendor_mismatch)
    ///   3. LoadLibraryW("vpl.dll") 优先 → 失败回退 LoadLibraryW("libmfx.dll")
    ///      → 都缺失 MC_ERR_NO_HARDWARE (sdk_missing)
    ///   4. GetProcAddress 关键入口 → MC_ERR_INTERNAL (sdk_init_failed)
    ///   5. Phase 6b: 创建 session / decoder → 实装中,当前 stub 返
    ///      MC_ERR_UNSUPPORTED (sdk_decode_pending) 让 controller 降到档 2/3
    mc_status_t start() noexcept;

    void        submit(std::vector<uint8_t> au_bytes, int64_t pts_us,
                        int64_t arrival_qpc_ns = 0) noexcept;
    void        flush() noexcept;
    void        stop() noexcept;

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

    [[nodiscard]] static bool probe_sdk_dll_present() noexcept;
    [[nodiscard]] static const char* reason_label(StartReason r) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_CODEC_ONEVPL_H_
