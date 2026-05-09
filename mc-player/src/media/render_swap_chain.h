/*
 * Swap chain 抽象 — 4 档（COMPAT / BALANCED / EXTREME / ULTIMATE_DCOMP）的统一接口。
 *
 * 不变量：
 *   - ALLOW_TEARING 必须在 Create / ResizeBuffers / Present 三处一致出现（ADD §5.10.3）
 *   - ResizeBuffers 前必先 ImmediateContext::ClearState 并显式释放所有引用 back buffer 的 SRV/RTV/UAV
 *
 * 实际 PresentMode 由 PresentMon 验证，stats 透出真实值（§5.10.4）。
 */

#ifndef MC_PLAYER_MEDIA_RENDER_SWAP_CHAIN_H_
#define MC_PLAYER_MEDIA_RENDER_SWAP_CHAIN_H_

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

#include "mc-player/mc_player_types.h"
#include "mc-player/mc_player_stats.h"

namespace mcp::media {

struct SwapChainCreateInfo {
    Microsoft::WRL::ComPtr<ID3D11Device>    device;
    HWND                                    hwnd            = nullptr;
    uint32_t                                width           = 0;
    uint32_t                                height          = 0;
    DXGI_FORMAT                             format          = DXGI_FORMAT_R8G8B8A8_UNORM;
    mc_render_profile_t                     profile_hint    = MC_RENDER_PROFILE_AUTO;
};

class SwapChain {
public:
    explicit SwapChain(SwapChainCreateInfo info);
    ~SwapChain();

    SwapChain(const SwapChain&)            = delete;
    SwapChain& operator=(const SwapChain&) = delete;

    mc_status_t create() noexcept;

    /// resize 前必走的清场（ClearState + 释放所有 view）；caller 提供回调释放业务侧 view。
    using ClearViewsFn = void (*)(void* user);
    mc_status_t resize(uint32_t width, uint32_t height,
                        ClearViewsFn clear_cb, void* clear_user) noexcept;

    /// 等待 Present 调度时机；返回 true = 可 Present。
    bool wait_for_frame_latency(uint32_t timeout_ms) noexcept;

    /// 单次 Present。
    /// sync_interval:DXGI Present1 SyncInterval。
    ///   0 = 立即 Present(不锁 vsync);1 = 等 1 vsync;2 = 等 2 vsync(30fps@60Hz frame doubling)。
    /// allow_tearing:仅 sync_interval=0 时生效(Present1 限制 SyncInterval>0 不能 ALLOW_TEARING)。
    mc_status_t present(uint32_t sync_interval, bool allow_tearing) noexcept;

    [[nodiscard]] mc_render_profile_t active_profile() const noexcept { return active_profile_; }
    [[nodiscard]] mc_present_mode_t   active_present_mode() const noexcept { return active_present_mode_; }

    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer() const noexcept;

private:
    SwapChainCreateInfo                          info_;
    Microsoft::WRL::ComPtr<IDXGIFactory6>        factory_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3>      swap_chain_;
    HANDLE                                       waitable_      = nullptr;
    mc_render_profile_t                          active_profile_     = MC_RENDER_PROFILE_AUTO;
    mc_present_mode_t                            active_present_mode_= MC_PRESENT_MODE_UNKNOWN;
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_RENDER_SWAP_CHAIN_H_
