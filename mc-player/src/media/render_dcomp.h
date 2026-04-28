/*
 * DirectComposition wrapper — ULTIMATE_DCOMP 档下的多 visual 编排（ADD §5.10.2）。
 *
 * Visual 三件套：
 *   - video visual：ALLOW_TEARING，VRR-friendly
 *   - hud visual  ：vsync，与 video plane 解耦
 *   - status visual：长时 freeze 时每秒 redraw 一次（§5.10.5 watchdog）
 *
 * 每个 visual 是否拿独立 hardware plane 由 DWM + 显卡驱动综合判定（受 Z-order /
 * 像素格式 / 缩放 / alpha 约束）；ADR-008 明示「不当作必胜档」。
 */

#ifndef MC_PLAYER_MEDIA_RENDER_DCOMP_H_
#define MC_PLAYER_MEDIA_RENDER_DCOMP_H_

#include <Windows.h>
#include <dcomp.h>
#include <wrl/client.h>

#include "mc-player/mc_player_types.h"

namespace mcp::media {

class DcompRoot {
public:
    DcompRoot() = default;
    ~DcompRoot() = default;

    mc_status_t create(HWND hwnd) noexcept;
    void destroy() noexcept;

    [[nodiscard]] Microsoft::WRL::ComPtr<IDCompositionDevice>  device() const noexcept { return device_; }
    [[nodiscard]] Microsoft::WRL::ComPtr<IDCompositionVisual>  video_visual()  const noexcept { return video_visual_; }
    [[nodiscard]] Microsoft::WRL::ComPtr<IDCompositionVisual>  hud_visual()    const noexcept { return hud_visual_; }
    [[nodiscard]] Microsoft::WRL::ComPtr<IDCompositionVisual>  status_visual() const noexcept { return status_visual_; }

    /// T5 单点 commit — 由 PresentEpoch 调用。
    mc_status_t commit() noexcept;

private:
    Microsoft::WRL::ComPtr<IDCompositionDevice>  device_;
    Microsoft::WRL::ComPtr<IDCompositionTarget>  target_;
    Microsoft::WRL::ComPtr<IDCompositionVisual>  root_visual_;
    Microsoft::WRL::ComPtr<IDCompositionVisual>  video_visual_;
    Microsoft::WRL::ComPtr<IDCompositionVisual>  hud_visual_;
    Microsoft::WRL::ComPtr<IDCompositionVisual>  status_visual_;
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_RENDER_DCOMP_H_
