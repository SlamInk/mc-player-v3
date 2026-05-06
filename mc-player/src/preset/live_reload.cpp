#include "preset/live_reload.h"

#include <atomic>
#include <thread>

#include "pal/clock.h"
#include "pal/log.h"
#include "pal/metric.h"
#include "preset/preset_apply.h"
#include "preset/preset_definitions.h"
#include "preset/preset_selector.h"

namespace mcp::preset {

namespace {

constexpr int64_t kStableWindowNs = 60LL * 1'000'000'000LL;     // plan §10 升档试探窗口

}  // namespace

struct LiveReload::Impl {
    OscillationGuard       guard;
    std::atomic<bool>      running{false};
    std::thread            watchdog;
    std::atomic<int64_t>   last_decode_error_ns{0};
    std::atomic<int64_t>   last_present_underrun_ns{0};
    std::atomic<int64_t>   last_tearing_ns{0};
    std::atomic<int64_t>   last_significant_event_ns{0};     // 任一降级信号触发时刻
    std::atomic<double>    latest_loss_rate{0.0};
    std::atomic<int64_t>   latest_rtt_ns{0};
    probe::CapabilitySnapshot snap;
    std::mutex             snap_mu;

    static mc_preset_id_t downgrade_one(mc_preset_id_t cur) noexcept {
        switch (cur) {
            case MC_PRESET_SDI_REPLACEMENT: return MC_PRESET_REALTIME_LAN;
            case MC_PRESET_REALTIME_LAN:    return MC_PRESET_STREAMING_WIFI;
            case MC_PRESET_STREAMING_WIFI:  return MC_PRESET_WAN_FALLBACK;
            case MC_PRESET_WAN_FALLBACK:    return MC_PRESET_SAFE_MODE;
            default:                         return MC_PRESET_SAFE_MODE;
        }
    }

    void apply_with_telemetry(mc_preset_id_t target, const char* reason) noexcept {
        pal::metric::ScopedTimer t{
            pal::metric::Registry::instance().timer("mc.preset.reload_latency_ns")};
        char metric_name[160];
        std::snprintf(metric_name, sizeof(metric_name),
                      "mc.preset.reload_event.reason.%s.target.%d", reason,
                      static_cast<int>(target));
        pal::metric::Registry::instance().counter(metric_name).inc();
        pal::metric::Registry::instance().counter("mc.preset.reload_count").inc();
        (void)apply_preset(preset_definition(target));
        MCP_LOGF(pal::LogLevel::info,
                 "LiveReload: reloaded preset target=%d reason=%s",
                 static_cast<int>(target), reason);
    }

    void on_signal(int64_t now_ns, const char* reason) noexcept {
        last_significant_event_ns.store(now_ns, std::memory_order_release);
        const auto cur = static_cast<mc_preset_id_t>(
            pal::metric::Registry::instance().gauge("mc.preset.active_id").value());
        const auto target = downgrade_one(cur);
        if (target != cur) {
            guard.record_downgrade(now_ns);
            apply_with_telemetry(target, reason);
        }
    }

    void watchdog_loop() noexcept {
        while (running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (!running.load(std::memory_order_acquire)) break;

            const int64_t now = pal::Clock::now_ns();
            const int64_t last_event = last_significant_event_ns.load(std::memory_order_acquire);
            if (last_event == 0 || (now - last_event) < kStableWindowNs) continue;
            if (guard.upgrade_locked(now)) continue;

            // 60s 稳定 + 振荡未锁 → 试升一档(用 selector 重选)。
            probe::CapabilitySnapshot s_copy;
            { std::scoped_lock lk{snap_mu}; s_copy = snap; }
            const auto current = static_cast<mc_preset_id_t>(
                pal::metric::Registry::instance().gauge("mc.preset.active_id").value());
            const auto best = select_preset(s_copy);
            if (static_cast<int>(best) < static_cast<int>(current) && best != MC_PRESET_NONE) {
                guard.record_upgrade(now);
                apply_with_telemetry(best, "self_upgrade");
            }
        }
    }
};

LiveReload::LiveReload() noexcept : impl_{std::make_unique<Impl>()} {}

LiveReload::~LiveReload() { stop(); }

void LiveReload::start() noexcept {
    if (impl_->running.exchange(true, std::memory_order_acq_rel)) return;
    impl_->watchdog = std::thread([this] { impl_->watchdog_loop(); });
}

void LiveReload::stop() noexcept {
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) return;
    if (impl_->watchdog.joinable()) impl_->watchdog.join();
}

void LiveReload::on_loss_rate_changed(double loss_rate_short) noexcept {
    impl_->latest_loss_rate.store(loss_rate_short, std::memory_order_relaxed);
    if (loss_rate_short >= 0.03) {
        impl_->on_signal(pal::Clock::now_ns(), "loss");
    }
}

void LiveReload::on_rtt_changed_ns(int64_t rtt_p95_ns) noexcept {
    impl_->latest_rtt_ns.store(rtt_p95_ns, std::memory_order_relaxed);
    if (rtt_p95_ns >= 100'000'000LL) {     // 100ms
        impl_->on_signal(pal::Clock::now_ns(), "rtt");
    }
}

void LiveReload::on_decoder_error() noexcept {
    impl_->last_decode_error_ns.store(pal::Clock::now_ns(), std::memory_order_release);
    impl_->on_signal(pal::Clock::now_ns(), "decoder_error");
}

void LiveReload::on_present_underrun() noexcept {
    impl_->last_present_underrun_ns.store(pal::Clock::now_ns(), std::memory_order_release);
    impl_->on_signal(pal::Clock::now_ns(), "present_underrun");
}

void LiveReload::on_tearing_observed() noexcept {
    impl_->last_tearing_ns.store(pal::Clock::now_ns(), std::memory_order_release);
    impl_->on_signal(pal::Clock::now_ns(), "tearing");
}

void LiveReload::update_snapshot(const probe::CapabilitySnapshot& s) noexcept {
    std::scoped_lock lk{impl_->snap_mu};
    impl_->snap = s;
}

}  // namespace mcp::preset
