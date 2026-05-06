#include "media/frame_validity_gate.h"

#include "pal/etw_provider.h"
#include "pal/log.h"
#include "pal/metric.h"

namespace mcp::media {

namespace {

// 性能量度规范 §4.1 六 bit drop counter（与 mc_stats_t.gate_drops_* 同源,通过 metric
// Registry 暴露给 dump_stats / 1Hz 聚合;hot path atomic 字段保留作低开销快照）。
mcp::pal::metric::Counter& gate_counter(const char* name) {
    return mcp::pal::metric::Registry::instance().counter(name);
}

// 性能量度规范 §2.5 mc.sync.gate_wait_ns。
mcp::pal::metric::Timer& gate_wait_timer() {
    return mcp::pal::metric::Registry::instance().timer("mc.sync.gate_wait_ns");
}

mcp::pal::metric::Counter& gate_emit_counter() {
    return mcp::pal::metric::Registry::instance().counter("mc.gate.emit_count");
}

mcp::pal::metric::Counter& gate_tainted_enter_counter() {
    return mcp::pal::metric::Registry::instance().counter("mc.gate.tainted_enter_count");
}

mcp::pal::metric::Gauge& gate_tainted_state_gauge() {
    return mcp::pal::metric::Registry::instance().gauge("mc.gate.tainted_state");
}

}  // namespace

FrameValidityGate::FrameValidityGate(EmitFn emit) noexcept : emit_{std::move(emit)} {}

mc_gate_poison_source_t FrameValidityGate::pick_source_from_mask(uint32_t missing_mask) noexcept {
    // 优先级：refs > recovery > params > reorder > fence > color。
    // refs/recovery 通常是参考链事件的最直接信号，先归类。
    if (missing_mask & kValidityRefs)     return MC_GATE_POISON_REFS_MISSING;
    if (missing_mask & kValidityRecovery) return MC_GATE_POISON_REFS_MISSING;
    if (missing_mask & kValidityParams)   return MC_GATE_POISON_PARAMS_MISSING;
    if (missing_mask & kValidityReorder)  return MC_GATE_POISON_REORDER_MISSING;
    if (missing_mask & kValidityFence)    return MC_GATE_POISON_FENCE_MISSING;
    if (missing_mask & kValidityColor)    return MC_GATE_POISON_COLOR_MISSING;
    return MC_GATE_POISON_NONE;
}

bool FrameValidityGate::admit(const VideoFrame& frame) noexcept {
    // 性能量度规范 §2.5：mc.sync.gate_wait_ns 记录 admit 整个判定 + emit 耗时。
    pal::metric::ScopedTimer gate_timer{gate_wait_timer()};

    // 状态机决策仅在锁内完成，emit 出锁后调用以避免 sink 回调重入死锁。
    enum class Decision { drop, emit_clean, emit_anchor_recover } decision = Decision::drop;
    {
        std::lock_guard<std::mutex> lk{mu_};

        // 1) decode_error 直接进入污染态。整条恢复链路靠下一 refresh anchor 重启。
        if (frame.decode_error) {
            if (!poisoned_) {
                poisoned_      = true;
                poison_source_ = MC_GATE_POISON_DECODE_ERROR;
                poison_enter_count_.fetch_add(1, std::memory_order_relaxed);
                gate_tainted_enter_counter().inc();
                gate_tainted_state_gauge().set(1);
                MCP_LOGF(pal::LogLevel::warn,
                         "FrameValidityGate: enter poisoned (decode_error) pts=%lld",
                         static_cast<long long>(frame.pts_us));
            }
            record_drop_locked(kValidityRefs | kValidityRecovery, frame.pts_us);
            poison_drops_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // 2) 检查 frame 级 bit。任一缺失既计入对应 drop 桶，也使 gate 进入污染态。
        const uint32_t missing = kValidityAll & ~frame.validity_mask;
        if (missing != 0) {
            if (!poisoned_) {
                poisoned_      = true;
                poison_source_ = pick_source_from_mask(missing);
                poison_enter_count_.fetch_add(1, std::memory_order_relaxed);
                gate_tainted_enter_counter().inc();
                gate_tainted_state_gauge().set(1);
                MCP_LOGF(pal::LogLevel::warn,
                         "FrameValidityGate: enter poisoned (missing=0x%02X) pts=%lld",
                         missing, static_cast<long long>(frame.pts_us));
            }
            record_drop_locked(missing, frame.pts_us);
            poison_drops_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // 3) 不处于污染态 + 全 bit set → emit。
        if (!poisoned_) {
            decision = Decision::emit_clean;
        } else if (frame.is_keyframe) {
            // 4) 处于污染态：仅 anchor 帧（IDR/IRAP/GDR-complete）可解 freeze；
            //    anchor 帧自身仍受其他 bit 校验（步骤 2 已保证全 bit set 才走到这里）。
            const mc_gate_poison_source_t prev = poison_source_;
            poisoned_      = false;
            poison_source_ = MC_GATE_POISON_NONE;
            gate_tainted_state_gauge().set(0);
            MCP_LOGF(pal::LogLevel::info,
                     "FrameValidityGate: leave poisoned (anchor) prev_source=%d pts=%lld",
                     static_cast<int>(prev), static_cast<long long>(frame.pts_us));
            decision = Decision::emit_anchor_recover;
        } else {
            // 处于污染态但不是 anchor → 丢。计入 refs+recovery 桶。
            record_drop_locked(kValidityRefs | kValidityRecovery, frame.pts_us);
            poison_drops_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    if (emit_) emit_(frame);
    gate_emit_counter().inc();
    (void)decision;
    return true;
}

void FrameValidityGate::mark_poisoned(mc_gate_poison_source_t source) noexcept {
    std::lock_guard<std::mutex> lk{mu_};
    if (poisoned_ && poison_source_ == source) return;
    poisoned_      = true;
    poison_source_ = source;
    poison_enter_count_.fetch_add(1, std::memory_order_relaxed);
    gate_tainted_enter_counter().inc();
    gate_tainted_state_gauge().set(1);
    MCP_LOGF(pal::LogLevel::warn,
             "FrameValidityGate: enter poisoned (external source=%d)",
             static_cast<int>(source));
}

bool FrameValidityGate::is_poisoned() const noexcept {
    std::lock_guard<std::mutex> lk{mu_};
    return poisoned_;
}

void FrameValidityGate::record_drop_locked(uint32_t missing_mask, int64_t pts_us) noexcept {
    // 性能量度规范 §4.1 六 bit drop counter（atomic fast-path + metric Registry sync）。
    if (missing_mask & kValidityRefs) {
        drops_refs_.fetch_add(1, std::memory_order_relaxed);
        gate_counter("mc.gate.drop_refs_resolved_count").inc();
    }
    if (missing_mask & kValidityParams) {
        drops_params_.fetch_add(1, std::memory_order_relaxed);
        gate_counter("mc.gate.drop_params_present_count").inc();
    }
    if (missing_mask & kValidityRecovery) {
        drops_recovery_.fetch_add(1, std::memory_order_relaxed);
        gate_counter("mc.gate.drop_recovery_complete_count").inc();
    }
    if (missing_mask & kValidityColor) {
        drops_color_.fetch_add(1, std::memory_order_relaxed);
        gate_counter("mc.gate.drop_color_meta_known_count").inc();
    }
    if (missing_mask & kValidityReorder) {
        drops_reorder_.fetch_add(1, std::memory_order_relaxed);
        gate_counter("mc.gate.drop_reorder_resolved_count").inc();
    }
    if (missing_mask & kValidityFence) {
        drops_fence_.fetch_add(1, std::memory_order_relaxed);
        gate_counter("mc.gate.drop_gpu_fence_signaled_count").inc();
    }
    last_drop_pts_us_.store(pts_us, std::memory_order_relaxed);

    MCP_LOGF(pal::LogLevel::trace,
             "FrameValidityGate drop pts=%lld missing=0x%02X",
             static_cast<long long>(pts_us), missing_mask);
}

void FrameValidityGate::fill_stats(mc_stats_t& s) const noexcept {
    s.gate_drops_refs       = drops_refs_.load(std::memory_order_relaxed);
    s.gate_drops_params     = drops_params_.load(std::memory_order_relaxed);
    s.gate_drops_recovery   = drops_recovery_.load(std::memory_order_relaxed);
    s.gate_drops_color      = drops_color_.load(std::memory_order_relaxed);
    s.gate_drops_reorder    = drops_reorder_.load(std::memory_order_relaxed);
    s.gate_drops_fence      = drops_fence_.load(std::memory_order_relaxed);
    s.gate_last_drop_pts_us = last_drop_pts_us_.load(std::memory_order_relaxed);

    bool poisoned_now;
    mc_gate_poison_source_t source_now;
    {
        std::lock_guard<std::mutex> lk{mu_};
        poisoned_now = poisoned_;
        source_now   = poison_source_;
    }
    s.gate_poisoned             = poisoned_now ? 1u : 0u;
    s.gate_last_poison_source   = source_now;
    s.gate_poison_enter_count   = poison_enter_count_.load(std::memory_order_relaxed);
    s.gate_poison_drops         = poison_drops_.load(std::memory_order_relaxed);
}

void FrameValidityGate::reset() noexcept {
    {
        std::lock_guard<std::mutex> lk{mu_};
        poisoned_      = false;
        poison_source_ = MC_GATE_POISON_NONE;
    }
    drops_refs_.store(0,     std::memory_order_relaxed);
    drops_params_.store(0,   std::memory_order_relaxed);
    drops_recovery_.store(0, std::memory_order_relaxed);
    drops_color_.store(0,    std::memory_order_relaxed);
    drops_reorder_.store(0,  std::memory_order_relaxed);
    drops_fence_.store(0,    std::memory_order_relaxed);
    last_drop_pts_us_.store(0, std::memory_order_relaxed);
    poison_enter_count_.store(0, std::memory_order_relaxed);
    poison_drops_.store(0,       std::memory_order_relaxed);
}

}  // namespace mcp::media
