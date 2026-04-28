#include "controller/adapter_picker.h"

namespace mcp::controller {

namespace {

bool adapter_supports_codec(const pal::AdapterCaps& a, mc_video_codec_t codec) noexcept {
    switch (codec) {
        case MC_VIDEO_CODEC_H264: return a.h264_supported;
        case MC_VIDEO_CODEC_H265: return a.hevc_main_supported || a.hevc_main10_supported;
        case MC_VIDEO_CODEC_AV1:  return a.av1_supported;
        default:                  return false;
    }
}

uint64_t adapter_score(const pal::AdapterCaps& a) noexcept {
    // 简单评分：dedicated VRAM 主导（dGPU > Iris Xe > 老 iGPU）。
    return a.dedicated_video_memory + (a.dual_bind_supported ? (uint64_t)1 << 30 : 0);
}

}  // namespace

std::optional<AdapterPick> AdapterPicker::pick(const pal::DxgiCapsProbe& caps,
                                                HWND hwnd,
                                                mc_video_codec_t codec,
                                                uint32_t override_luid_low,
                                                int32_t  override_luid_high) noexcept {
    // O5: 用户显式 LUID 覆盖
    if (override_luid_low != 0 || override_luid_high != 0) {
        LUID lu;
        lu.LowPart  = override_luid_low;
        lu.HighPart = override_luid_high;
        if (auto* a = caps.find_by_luid(lu)) {
            AdapterPick p;
            p.luid                       = a->luid;
            p.using_user_override        = true;
            p.requires_libcodec_fallback = !adapter_supports_codec(*a, codec);
            return p;
        }
    }

    // O1-O2: HWND 跟随
    if (auto* a = caps.find_by_hwnd(hwnd)) {
        if (adapter_supports_codec(*a, codec)) {
            AdapterPick p;
            p.luid = a->luid;
            return p;
        }
    }

    // O3: 按能力排序
    const pal::AdapterCaps* best = nullptr;
    uint64_t                best_score = 0;
    for (const auto& a : caps.adapters()) {
        if (a.is_software) continue;
        if (!adapter_supports_codec(a, codec)) continue;
        const uint64_t sc = adapter_score(a);
        if (sc > best_score) {
            best_score = sc;
            best       = &a;
        }
    }
    if (best) {
        AdapterPick p;
        p.luid = best->luid;
        return p;
    }

    // O4: 全失败 → 走软解兜底
    AdapterPick p;
    if (!caps.adapters().empty()) {
        p.luid = caps.adapters().front().luid;
    }
    p.requires_libcodec_fallback = true;
    return p;
}

}  // namespace mcp::controller
