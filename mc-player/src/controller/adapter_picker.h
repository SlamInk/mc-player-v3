/*
 * 智能 Adapter 选择（ADD §7.3 / ADR-007）。
 *
 *   O1. HWND → MonitorFromWindow → owning adapter（"preferred"）
 *   O2. caps[preferred] 满足 codec → 使用 preferred（同 device 直 scan-out，零跨 device 拷贝）
 *   O3. 否则按能力排序：dGPU > Iris Xe > 老 iGPU
 *   O4. 仍无满足 → mc-libcodec 软解
 *   O5. LUID 显式覆盖跳过 O1-O3
 */

#ifndef MC_PLAYER_CONTROLLER_ADAPTER_PICKER_H_
#define MC_PLAYER_CONTROLLER_ADAPTER_PICKER_H_

#include <Windows.h>

#include <optional>

#include "mc-player/mc_player_types.h"
#include "pal/dxgi_caps_probe.h"

namespace mcp::controller {

struct AdapterPick {
    LUID                            luid{};
    bool                            requires_libcodec_fallback = false;
    bool                            using_user_override        = false;
};

class AdapterPicker {
public:
    /// luid_low / luid_high == 0 时按 HWND 跟随。
    static std::optional<AdapterPick> pick(const pal::DxgiCapsProbe& caps,
                                            HWND hwnd,
                                            mc_video_codec_t codec,
                                            uint32_t override_luid_low,
                                            int32_t  override_luid_high) noexcept;
};

}  // namespace mcp::controller

#endif  // MC_PLAYER_CONTROLLER_ADAPTER_PICKER_H_
