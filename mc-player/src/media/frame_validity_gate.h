/*
 * Frame Validity Gate — 协议无关的输出门控（ADD §5.13）。
 *
 * 介于 codec bridge 与 render 之间。所有花屏预防机制（refs / GDR anchor / VUI / B 帧 reorder /
 * dual-bind fence）的执行点收口于此，避免散落在多个模块各自 freeze 各自兜底导致竞态。
 *
 * 状态生命周期（ADD §5.13）：
 *   两类生命周期：
 *   - Stream 级（一次锁定后常驻）：color_meta_known。
 *   - Frame 级 + 污染传播：refs / params / recovery / reorder / fence。任一参考链断裂事件
 *     触发后，所有后续帧默认 invalid，直至下一 refresh anchor（IDR/IRAP/recovery_point
 *     SEI w/ recovery_frame_cnt==0）才解除。
 *
 *   anchor 帧自身仍要求其他 frame 级 bit 也 set 才 emit，并不因为是 anchor 就豁免 fence
 *   或 reorder 校验。
 *
 * 判定规则：
 *   - decode_error          → 进入污染态（source=DECODE_ERROR），丢
 *   - 任一 bit 未 set        → 进入污染态（source=对应 bit），丢
 *   - 处于污染态 + 非 anchor → 丢（计入污染丢帧）
 *   - 处于污染态 + anchor + 全 bit set → 退出污染态，emit
 *   - 不处于污染态 + 全 bit set → emit
 *
 * 所有 drop 计数与最近一次 drop 的 PTS、当前污染状态、进入污染源都暴露给 stats（用于诊断
 * 「为什么这一帧没显示」以及「还要等多久才解 freeze」）。
 */

#ifndef MC_PLAYER_MEDIA_FRAME_VALIDITY_GATE_H_
#define MC_PLAYER_MEDIA_FRAME_VALIDITY_GATE_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

#include "media/frame.h"
#include "mc-player/mc_player_stats.h"

namespace mcp::media {

class FrameValidityGate {
public:
    using EmitFn = std::function<void(const VideoFrame&)>;

    explicit FrameValidityGate(EmitFn emit) noexcept;

    /// 入口：codec bridge 在解码出每个 frame 后调用一次。
    /// 返回 true = frame 已 emit；false = drop（被 bit 未满足或处于污染态拦下）。
    bool admit(const VideoFrame& frame) noexcept;

    /// 外部强制进入污染态（Device Lost / 网络重连 / soft adapter switch / ResizeBuffers
    /// 过渡等场景，由 Controller 在已知参考链失效时调用）。等下一 anchor 才解除。
    void mark_poisoned(mc_gate_poison_source_t source) noexcept;

    /// 把累计 drop 计数与污染状态填入 stats。线程安全。
    void fill_stats(mc_stats_t& s) const noexcept;

    /// 清零（Device Lost 全恢复完成 / flush 等）。
    /// 注意：reset() 是「重置 gate 自身计数与状态」，不等同于 mark_poisoned；
    /// 通常配合：先 reset → 再 mark_poisoned(EXTERNAL)，以保证下一 anchor 才解 freeze。
    void reset() noexcept;

    /// 当前是否处于污染态（仅诊断；状态机内部读由 admit 持锁完成）。
    [[nodiscard]] bool is_poisoned() const noexcept;

    /// 按 active preset 派生 6 bit strict 集合(plan §9.1 / capability_probe §6.3 +
    /// ADR-014 文末注解)。SDI_REPLACEMENT preset 的 NV12 直显路径无 SRV 消费方 +
    /// 无 RGB shader → strict 集合不含 color_meta_known / gpu_fence_signaled。
    /// 其他 preset 仍 6 bit 全 strict(默认)。
    void set_strict_color_fence(bool color_strict, bool fence_strict) noexcept;

private:
    void record_drop_locked(uint32_t missing_mask, int64_t pts_us) noexcept;
    static mc_gate_poison_source_t pick_source_from_mask(uint32_t missing_mask) noexcept;

    mutable std::mutex          mu_;
    EmitFn                      emit_;

    // 污染状态机（在 mu_ 保护下读写）
    bool                        poisoned_           = false;
    mc_gate_poison_source_t     poison_source_      = MC_GATE_POISON_NONE;

    // 计数：原子，便于 fill_stats 无锁读取
    std::atomic<uint64_t>       drops_refs_{0};
    std::atomic<uint64_t>       drops_params_{0};
    std::atomic<uint64_t>       drops_recovery_{0};
    std::atomic<uint64_t>       drops_color_{0};
    std::atomic<uint64_t>       drops_reorder_{0};
    std::atomic<uint64_t>       drops_fence_{0};
    std::atomic<int64_t>        last_drop_pts_us_{0};

    std::atomic<uint64_t>       poison_enter_count_{0};
    std::atomic<uint64_t>       poison_drops_{0};

    // Phase 9.1: SDI_REPLACEMENT NV12 直显路径下 color/fence bit 不参与 strict 判定。
    // 默认 true 保持 ADR-014 严格 6 bit;SDI preset 命中后 set_strict_color_fence(false, false)。
    std::atomic<bool>           strict_color_{true};
    std::atomic<bool>           strict_fence_{true};
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_FRAME_VALIDITY_GATE_H_
