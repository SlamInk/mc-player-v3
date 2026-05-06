/*
 * Present Epoch + Watchdog（ADD §5.10.5）— 陈旧区域防御。
 *
 * 单点刷新权威：
 *   T5（render thread）是唯一 IDCompositionDevice::Commit 调用方。video / hud / status
 *   三个 visual 的内容更新必须经 T5 队列汇总。
 *
 * Present Epoch ID：单调递增，每帧 +1。video swap chain Present 与 DCOMP commit 同 epoch
 * 配对，每个 epoch 只 commit 一次，避免「video 已 Present、HUD 未 commit」的撕裂半态。
 *
 * 陈旧区域 Watchdog：周期检查「最近一次 Present 时刻 vs 当前 epoch」。
 * 超过 N × frame_period 未推进 → 强制 redraw last-good 并重 commit；
 * 处理 WM_PAINT / DPI 变更 / 显示模式切换 / 设备切换期间的 DWM 缓存旧 visual。
 *
 * v1 验收口径「无渲染陈旧层叠加」由本模块的 watchdog + 单点 commit 一起保证。
 */

#ifndef MC_PLAYER_MEDIA_RENDER_PRESENT_EPOCH_H_
#define MC_PLAYER_MEDIA_RENDER_PRESENT_EPOCH_H_

#include <atomic>
#include <cstdint>
#include <functional>

namespace mcp::media {

class PresentEpoch {
public:
    using RedrawFn = std::function<void()>;
    using CommitFn = std::function<void()>;

    PresentEpoch(RedrawFn redraw, CommitFn commit) noexcept;

    /// T5 在每次 Present 完成后调用一次（同 epoch 内只能一次 commit）。
    void on_presented(int64_t now_ns) noexcept;

    /// 一个新 epoch 启动（每帧渲染入口）。
    [[nodiscard]] uint64_t begin_epoch() noexcept;

    /// Watchdog tick：T5 周期调，过期则触发 redraw + commit。
    /// frame_period_ns = 1/refresh，N 默认 3 倍。
    void tick(int64_t now_ns, int64_t frame_period_ns, int n_periods_threshold) noexcept;

    /// resume from freeze / device-lost / soft adapter switch — 必须显式 redraw 一次。
    void force_redraw() noexcept;

    [[nodiscard]] uint64_t skip_count() const noexcept { return skip_count_.load(std::memory_order_relaxed); }

    // Phase 9.4 子目标 4:Race-to-display Present(Reflex 风格)。
    //   一帧解码就绪即 Present,依赖 ALLOW_TEARING + VRR;非 VRR 路径 PresetApply
    //   graceful degrade 退到 vsync-aligned。
    enum class PresentMode : uint8_t {
        VsyncAligned    = 0,
        AllowTearing    = 1,
        RaceToDisplay   = 2,
    };
    void set_present_mode(PresentMode m) noexcept;
    [[nodiscard]] PresentMode present_mode() const noexcept {
        return present_mode_.load(std::memory_order_acquire);
    }

private:
    RedrawFn                redraw_;
    CommitFn                commit_;
    std::atomic<uint64_t>   epoch_id_{0};
    std::atomic<int64_t>    last_present_ns_{0};
    std::atomic<bool>       commit_pending_in_epoch_{false};
    std::atomic<uint64_t>   skip_count_{0};
    std::atomic<PresentMode> present_mode_{PresentMode::AllowTearing};
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_RENDER_PRESENT_EPOCH_H_
