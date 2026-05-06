/*
 * Preset Live Reload — 运行期持续适配(plan §10 / ADR-020)。
 *
 * 5 类降级信号(任一触发立即降一档 preset):
 *   - loss_rate_short >= 0.03(网络突丢)
 *   - rtt_p95_short >= 100ms(网络突阻塞)
 *   - decoder error rate > N/min(解码错误)
 *   - present underrun(渲染陈旧帧 watchdog 触发)
 *   - tearing 实测 > 阈值(VRR 失效)
 *
 * 升档试探:
 *   - 60s 全维稳定 + 0 tainted 事件 + oscillation_guard 不锁定 → 升一档
 *
 * Reload 原子性:
 *   - T0~T3 不中断
 *   - T4/T5 帧间缝隙换档
 *   - jitter 双缓冲交替读不丢帧
 *   - render_present_epoch reload = epoch 切换源,旧 epoch frame 兜底丢弃
 */

#ifndef MC_PLAYER_PRESET_LIVE_RELOAD_H_
#define MC_PLAYER_PRESET_LIVE_RELOAD_H_

#include <atomic>
#include <memory>

#include "mc-player/mc_player_types.h"
#include "preset/oscillation_guard.h"
#include "probe/capability_snapshot.h"

namespace mcp::preset {

class LiveReload {
public:
    LiveReload() noexcept;
    ~LiveReload();

    LiveReload(const LiveReload&)            = delete;
    LiveReload& operator=(const LiveReload&) = delete;

    /// 启动 60s 全维稳定 watchdog 主循环。
    void start() noexcept;
    void stop() noexcept;

    // 5 类降级信号订阅 — 由各对应模块 hook 调用(transport / codec / render)。
    void on_loss_rate_changed(double loss_rate_short) noexcept;
    void on_rtt_changed_ns(int64_t rtt_p95_ns) noexcept;
    void on_decoder_error() noexcept;
    void on_present_underrun() noexcept;
    void on_tearing_observed() noexcept;

    // 当前 capability snapshot(由 controller 周期性更新)。
    void update_snapshot(const probe::CapabilitySnapshot& snap) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp::preset

#endif  // MC_PLAYER_PRESET_LIVE_RELOAD_H_
