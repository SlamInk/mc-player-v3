/*
 * UI Overlay — D2D + DirectWrite 在视频 backbuffer 之上绘 HUD/UI（ADD §5.10 / §5.10.5）。
 *
 * 决策：UI 走底层渲染（Direct2D + DirectWrite over D3D11），不引入 WebView2 / HTML：
 *   - 共享 swap chain backbuffer，单次 Present 把视频 + UI 原子提交，无 plane 合成额外延迟；
 *   - D3D11 / D2D / DirectWrite 全部 OS 内置，符合 ADD §2 #10「平台原生优先」；
 *   - 跟视频走同一 ID3D11Device，零跨 device 拷贝、零进程间 IPC。
 *
 * 集成点：RenderD3d11::render_locked() 在视频帧 Draw 之后、Present 之前调用 render()；
 *          tick(now_ns) 每帧由 60Hz UI tick 驱动一次以推进动画时间基（cBreathe / cBlink /
 *          cLive / connecting elapsed counter / scan glow 等）。
 *
 * 强约束：
 *   - 必须跟 D3D11 device 共享，且 device 创建时必须 BIND BGRA_SUPPORT（controller.cpp 已开）。
 *   - ResizeBuffers / device-lost 全恢复前必须 unbind_backbuffer()，与 §5.10.3 ClearState 不变量协调。
 *   - 视觉对齐 mc-player-ui-ux/direction-c.jsx「黑白零噪声」主题；本轮先功能完整，drop-shadow
 *     / backdrop-filter / 文字渐变等精修档延后轮。
 */

#ifndef MC_PLAYER_MEDIA_UI_OVERLAY_H_
#define MC_PLAYER_MEDIA_UI_OVERLAY_H_

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <memory>
#include <string>

#include "mc-player/mc_player_stats.h"
#include "mc-player/mc_player_types.h"

namespace mcp::media {

/// Stage 状态对齐 direction-c.jsx 的 5 个视觉态。set_stage 线程安全。
enum class UiStage {
    empty       = 0,    // mc·player wordmark + 底部 spec rail（默认）
    connecting  = 1,    // 握手进度（96px elapsed + 进度条 + step log）
    playing     = 2,    // 视频区 + corner marks + URL bar + 大延时 HUD
    error       = 3,    // 大圆 X + 诊断三联
    add_modal   = 4,    // 添加流弹窗（遮罩 + 480 模态 + Name/URL/preflight）
};

class UiOverlay {
public:
    struct Config {
        Microsoft::WRL::ComPtr<ID3D11Device>    device;
    };

    explicit UiOverlay(Config cfg);
    ~UiOverlay();

    UiOverlay(const UiOverlay&)            = delete;
    UiOverlay& operator=(const UiOverlay&) = delete;

    /// 创建 D2D / DirectWrite factory + TextFormat + 时间基。
    HRESULT initialize() noexcept;

    /// 绑定 swap chain backbuffer 作为 D2D render target；ResizeBuffers 后再调用一次。
    HRESULT bind_backbuffer(Microsoft::WRL::ComPtr<ID3D11Texture2D> backbuffer) noexcept;

    /// 释放 RT；ResizeBuffers / device-lost 之前调用。factory / TextFormat 不释放。
    void unbind_backbuffer() noexcept;

    // ── 状态推送（线程安全，全部 partial-update） ─────────────────────────

    /// 切 stage。会刷新 stage_enter_time_ns 用于 elapsed 类动画。
    void set_stage(UiStage stage) noexcept;

    /// playing stage 是否叠加右上 stats drawer。
    void set_show_stats(bool show) noexcept;

    /// addModal 显隐（瞬态遮罩，独立于 stage 主状态）。
    void set_show_add_modal(bool show) noexcept;

    /// URL 输入显示值（CUrlBar 文本区域）。
    void set_url(std::wstring url) noexcept;

    /// playing stage 的延时 / fps / bitrate / 丢帧等。深拷一份。
    void set_stats(const mc_stats_t& stats) noexcept;

    /// stream_info：codec / 分辨率 / decoder kind / gpu_kind / adapter description。
    void set_stream_info(const mc_stream_info_t& info) noexcept;

    /// error stage 的 code / message / url（CONNECTION_TIMEOUT、no SDP received…）。
    void set_error(std::wstring code, std::wstring detail, std::wstring url) noexcept;

    [[nodiscard]] UiStage stage() const noexcept;

    /// 推进动画时间基（60Hz tick 调用）。线程安全。
    void tick(int64_t now_ns) noexcept;

    /// 在 backbuffer 上绘 UI。视频帧 Draw 之后、Present 之前调用。
    void render() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_UI_OVERLAY_H_
