/*
 * D3D11 Renderer — 4 档 Profile 编排 + NV12→RGB shader（ADD §5.10）。
 *
 * 输入 VideoFrame（dual-bind texture array slice 或 NV12 RAM）→ NV12→RGB pixel shader →
 * Swap chain back buffer → Present + ALLOW_TEARING（按档位决定）。
 *
 * 强约束（综合 ADD §5.10 / §5.13 / ADR-014）：
 *   - 帧 emit 前由 FrameValidityGate 校验六类 bit；本类只渲染已 admit 的帧。
 *   - Present 与 PresentEpoch 同 epoch 配对；T5 是唯一 commit 调用方。
 *   - dual-bind 路径：等 ID3D11Fence signal 后才采样 array slice。
 *   - YUV→RGB 矩阵按 ResolvedColor 选用；range 在 shader 内 unscale。
 */

#ifndef MC_PLAYER_MEDIA_RENDER_D3D11_H_
#define MC_PLAYER_MEDIA_RENDER_D3D11_H_

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <memory>

#include "media/frame.h"
#include "media/render_dcomp.h"
#include "media/render_present_epoch.h"
#include "media/render_swap_chain.h"
#include "media/ui_overlay.h"

namespace mcp::media {

class RenderD3d11 {
public:
    struct Config {
        Microsoft::WRL::ComPtr<ID3D11Device>    device;
        HWND                                    hwnd        = nullptr;
        mc_render_profile_t                     profile_hint= MC_RENDER_PROFILE_AUTO;
    };

    explicit RenderD3d11(Config cfg);
    ~RenderD3d11();

    RenderD3d11(const RenderD3d11&)            = delete;
    RenderD3d11& operator=(const RenderD3d11&) = delete;

    mc_status_t start() noexcept;
    void stop() noexcept;

    /// 由 FrameValidityGate emit 调用 — 已通过六类 bit 验证。
    void on_admitted(const VideoFrame& frame) noexcept;

    /// T5 watchdog tick；过期触发 force_redraw。
    void tick_watchdog() noexcept;

    /// 60Hz UI tick：推进 UI overlay 动画时间基；如果当前没有 last_good 视频帧（empty/connecting/
    /// error stage），强制 redraw_ui_only 把动画相位刷到屏。playing stage 由视频帧自然驱动。
    void tick_ui() noexcept;

    /// 暴露 UI overlay 指针给 Controller（set_stage / set_url / set_stats 等）。
    /// 仅 start() 之后非空；stop() 后为空。
    [[nodiscard]] UiOverlay* ui_overlay() noexcept;

    [[nodiscard]] mc_render_profile_t active_profile() const noexcept;
    [[nodiscard]] mc_present_mode_t   active_present_mode() const noexcept;

    [[nodiscard]] uint64_t present_count() const noexcept;
    [[nodiscard]] uint64_t skip_count()    const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_RENDER_D3D11_H_
