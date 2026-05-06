#include "preset/oscillation_guard.h"

#include "pal/log.h"
#include "pal/metric.h"

namespace mcp::preset {

void OscillationGuard::record_upgrade(int64_t now_ns) noexcept {
    last_upgrade_ns_.store(now_ns, std::memory_order_release);
    pal::metric::Registry::instance().counter("mc.preset.upgrade_count").inc();
}

void OscillationGuard::record_downgrade(int64_t now_ns) noexcept {
    pal::metric::Registry::instance().counter("mc.preset.downgrade_count").inc();
    const int64_t last_upgrade = last_upgrade_ns_.load(std::memory_order_acquire);
    if (last_upgrade != 0 && (now_ns - last_upgrade) < kOscillationWindowNs) {
        const uint32_t count = oscillation_count_.fetch_add(1, std::memory_order_acq_rel) + 1;
        last_oscillation_ns_.store(now_ns, std::memory_order_release);
        pal::metric::Registry::instance().gauge("mc.preset.oscillation_count")
            .set(static_cast<int64_t>(count));
        MCP_LOGF(pal::LogLevel::warn,
                 "OscillationGuard: oscillation #%u(downgrade within 30s of upgrade)",
                 count);
    }
}

bool OscillationGuard::upgrade_locked(int64_t now_ns) const noexcept {
    const uint32_t count = oscillation_count_.load(std::memory_order_acquire);
    if (count < kOscillationLockThreshold) return false;
    const int64_t last_osc = last_oscillation_ns_.load(std::memory_order_acquire);
    return last_osc != 0 && (now_ns - last_osc) < kLockoutWindowNs;
}

}  // namespace mcp::preset
