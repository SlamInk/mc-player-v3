/*
 * Oscillation Guard — 升降振荡保护(plan §10 / ADR-020)。
 *
 * 状态机:
 *   - 升档 → 30s 内再降 → oscillation_count++
 *   - oscillation_count >= 2 → 锁定 5min 禁升
 *   - 5min 窗口结束后 oscillation_count 衰减回 0
 *
 * 防止"网络抖动 → 降档 → 60s 稳定 → 升档 → 又抖动 → 降档"无限循环消耗 reload 成本。
 */

#ifndef MC_PLAYER_PRESET_OSCILLATION_GUARD_H_
#define MC_PLAYER_PRESET_OSCILLATION_GUARD_H_

#include <atomic>
#include <cstdint>

namespace mcp::preset {

class OscillationGuard {
public:
    OscillationGuard() noexcept = default;

    /// 升档发生 — 记录 timestamp。
    void record_upgrade(int64_t now_ns) noexcept;

    /// 降档发生 — 若距上次升档 < 30s 则 oscillation_count++。
    void record_downgrade(int64_t now_ns) noexcept;

    /// 当前是否处于"禁升锁定"窗口(oscillation_count >= 2 且 last_oscillation 5min 内)。
    [[nodiscard]] bool upgrade_locked(int64_t now_ns) const noexcept;

    [[nodiscard]] uint32_t oscillation_count() const noexcept {
        return oscillation_count_.load(std::memory_order_acquire);
    }

private:
    std::atomic<int64_t>  last_upgrade_ns_{0};
    std::atomic<int64_t>  last_oscillation_ns_{0};
    std::atomic<uint32_t> oscillation_count_{0};

    static constexpr int64_t kOscillationWindowNs = 30LL * 1'000'000'000LL;       // 30s
    static constexpr int64_t kLockoutWindowNs     = 5LL * 60'000'000'000LL;       // 5min
    static constexpr uint32_t kOscillationLockThreshold = 2;
};

}  // namespace mcp::preset

#endif  // MC_PLAYER_PRESET_OSCILLATION_GUARD_H_
