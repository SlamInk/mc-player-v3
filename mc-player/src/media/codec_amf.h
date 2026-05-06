/*
 * AMF — AMD Advanced Media Framework 直驱档 1 实装(ADR-015 §5.6.2.1)。
 *
 * 实装边界(Phase 7):
 *   本 commit 落地"结构性骨架 + 动态加载 + vendor 探测 + skip reason 分类"，
 *   镜像 Phase 5a NVDEC / Phase 6a oneVPL 模式。AMF ABI(AMFFactory / AMFContext /
 *   AMFComponent / AMFSurface 等)与 oneVPL 一样需要 ~500 行实装,留 Phase 7b。
 *
 *   Phase 7 现可验路径：
 *     - VendorId == 0x1002 (AMD) 检测
 *     - LoadLibraryW("amfrt64.dll") 探测
 *     - 关键 AMF 入口 GetProcAddress (AMFInit / AMFQueryVersion)
 *
 *   Phase 7b 待补：
 *     - AMFInit + AMFFactory::CreateContext + InitDX11(d3d11_device)
 *     - CreateComponent(AMFVideoDecoderUVD_H264_AVC / _H265_HEVC)
 *     - AMF_VIDEO_DECODER_REORDER_MODE = AMF_VIDEO_DECODER_MODE_LOW_LATENCY
 *     - Submit/QueryOutput 异步管线 + AMFSurface → D3D11 texture
 */

#ifndef MC_PLAYER_MEDIA_CODEC_AMF_H_
#define MC_PLAYER_MEDIA_CODEC_AMF_H_

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

class CodecAmf {
public:
    using EmitFn = std::function<void(VideoFrame&&)>;

    struct Config {
        mc_video_codec_t                       codec   = MC_VIDEO_CODEC_H264;
        Microsoft::WRL::ComPtr<ID3D11Device>   device;
        EmitFn                                 emit;
    };

    explicit CodecAmf(Config cfg);
    ~CodecAmf();

    CodecAmf(const CodecAmf&)            = delete;
    CodecAmf& operator=(const CodecAmf&) = delete;

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

#endif  // MC_PLAYER_MEDIA_CODEC_AMF_H_
