#include "media/render_present_epoch.h"

namespace mcp::media {

PresentEpoch::PresentEpoch(RedrawFn redraw, CommitFn commit) noexcept
    : redraw_{std::move(redraw)}, commit_{std::move(commit)} {}

uint64_t PresentEpoch::begin_epoch() noexcept {
    commit_pending_in_epoch_.store(true, std::memory_order_release);
    return epoch_id_.fetch_add(1, std::memory_order_acq_rel) + 1;
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
