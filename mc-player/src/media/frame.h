/*
 * Frame — 编解码-渲染之间的数据载体。
 *
 * 设计：硬解路径下 video frame 携带 D3D11 texture array slice 引用；软解路径下携带 NV12 RAM。
 * Frame Validity Gate (§5.13) 在 emit 给 renderer 前必须六类 bit 全部 set。
 *
 * Frame 是值语义：拷贝廉价（D3D11 texture 是 ComPtr 引用计数）。生命周期由 SPSC 队列驱动。
 */

#ifndef MC_PLAYER_MEDIA_FRAME_H_
#define MC_PLAYER_MEDIA_FRAME_H_

#include <Windows.h>
#include <d3d11.h>
#include <d3d11_4.h>      // ID3D11Fence (D3D11.3+)
#include <wrl/client.h>

#include <cstdint>
#include <vector>

#include "mc-player/mc_player_types.h"

namespace mcp::media {

/// Validity Mask — Frame Validity Gate 的六类 bit（ADD §5.13）。
enum ValidityBit : uint32_t {
    kValidityRefs       = 1u << 0,       // refs_resolved      (DPB 无 gap)
    kValidityParams     = 1u << 1,       // params_present     (SPS/PPS 缓存)
    kValidityRecovery   = 1u << 2,       // recovery_complete  (anchor 已就位)
    kValidityColor      = 1u << 3,       // color_meta_known
    kValidityReorder    = 1u << 4,       // reorder_resolved   (B 帧已排序)
    kValidityFence      = 1u << 5,       // gpu_fence_signaled (dual-bind 写完成)
    kValidityAll        = 0x3F,
};

enum class FrameSource {
    mft_dxva,          // hardware decode → D3D11 texture array slice
    mft_software,      // OS software MFT → CPU NV12
    libcodec_software, // mc-libcodec → CPU NV12
};

struct VideoFrame {
    int64_t                                 pts_us              = 0;     // RTP→NTP 后归一化
    uint32_t                                width               = 0;
    uint32_t                                height              = 0;

    FrameSource                             source              = FrameSource::mft_dxva;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> dxva_texture;                  // 硬解路径
    uint32_t                                dxva_array_slice    = 0;
    Microsoft::WRL::ComPtr<ID3D11Fence>     dxva_fence;                    // dual-bind fence
    uint64_t                                dxva_fence_value    = 0;

    std::vector<uint8_t>                    nv12_ram;                       // 软解路径 (Y+UV interleaved)
    uint32_t                                nv12_y_stride       = 0;
    uint32_t                                nv12_uv_stride      = 0;

    mc_color_primaries_t                    color_primaries     = MC_COLOR_PRIMARIES_AUTO;
    mc_color_range_t                        color_range         = MC_COLOR_RANGE_AUTO;
    mc_color_matrix_t                       color_matrix        = MC_COLOR_MATRIX_AUTO;

    uint32_t                                validity_mask       = 0;       // ValidityBit 组合
    bool                                    is_keyframe         = false;
    bool                                    decode_error        = false;
};

struct AudioFrame {
    int64_t              pts_us         = 0;
    uint32_t             sample_rate_hz = 0;
    uint32_t             channels       = 0;
    std::vector<float>   pcm_interleaved;                       // 32-bit float interleaved
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_FRAME_H_
