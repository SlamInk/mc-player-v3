#include "media/render_present_epoch.h"

#include "pal/metric.h"

namespace mcp::media {

namespace {

// 性能量度规范 §7.1：epoch id / commit pair / watchdog 三组状态。
// 通过 metric Registry 聚合给 dump_stats / 1Hz 上报；hot path 内仍走 atomic 字段。
mcp::pal::metric::Counter& epoch_pair_skew_counter() {
    return mcp::pal::metric::Registry::instance()
        .counter("mc.render.epoch_pair_skew_count");
}
mcp::pal::metric::Counter& watchdog_redraw_counter() {
    return mcp::pal::metric::Registry::instance()
        .counter("mc.render.watchdog_redraw_count");
}
mcp::pal::metric::Gauge& present_epoch_id_gauge() {
    return mcp::pal::metric::Registry::instance()
        .gauge("mc.render.present_epoch_id");
}

}  // namespace

PresentEpoch::PresentEpoch(RedrawFn redraw, CommitFn commit) noexcept
    : redraw_{std::move(redraw)}, commit_{std::move(commit)} {}

uint64_t PresentEpoch::begin_epoch() noexcept {
    // exchange 同时 set 并取旧值。旧值 == true 即上一 epoch 的 commit 漏发：
    // ADD §5.10.5 不变量违反 → mc.render.epoch_pair_skew_count
    // (性能量度规范 §7.1 / §11.1 必达指标 = 0 永久)。
    const bool prev_pending = commit_pending_in_epoch_.exchange(
        true, std::memory_order_acq_rel);
    if (prev_pending) {
        epoch_pair_skew_counter().inc();
    }
    const uint64_t id = epoch_id_.fetch_add(1, std::memory_order_acq_rel) + 1;
    present_epoch_id_gauge().set(static_cast<int64_t>(id));
    return id;
}

void PresentEpoch::on_presented(int64_t now_ns) noexcept {
    // 每个 epoch 只允许一次 commit，避免 video/HUD 半态撕裂。
    bool expected = true;
    if (commit_pending_in_epoch_.compare_exchange_strong(expected, false,
                                                          std::memory_order_acq_rel)) {
        if (commit_) commit_();
    }
    last_present_ns_.store(now_ns, std::memory_order_release);
}

void PresentEpoch::tick(int64_t now_ns, int64_t frame_period_ns, int n_periods_threshold) noexcept {
    const int64_t last = last_present_ns_.load(std::memory_order_acquire);
    if (last == 0) return;
    const int64_t threshold_ns = frame_period_ns * static_cast<int64_t>(n_periods_threshold);
    if (now_ns - last > threshold_ns) {
        skip_count_.fetch_add(1, std::memory_order_relaxed);
        // 性能量度规范 §7.1 mc.render.watchdog_redraw_count；§13 告警阈值
        // (P1: rate >0.1 Hz / P2: rate >1 Hz)。
        watchdog_redraw_counter().inc();
        force_redraw();
    }
}

void PresentEpoch::force_redraw() noexcept {
    if (redraw_) redraw_();
    bool expected = true;
    if (commit_pending_in_epoch_.compare_exchange_strong(expected, false,
                                                          std::memory_order_acq_rel)) {
        if (commit_) commit_();
    } else {
        // commit 已发生过：再发一次，确保 DWM 缓存的旧 visual 失效。
        if (commit_) commit_();
    }
}

}  // namespace mcp::media
