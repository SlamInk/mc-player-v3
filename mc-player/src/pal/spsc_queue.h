/*
 * 单生产单消费 (SPSC) 无锁环形队列。
 *
 * ADD §6.1 强约束：
 *   - cache-line padding 必做（false-sharing 性能降 3-5×）
 *   - 深度极低；满时丢最老（由 caller 决定丢弃策略，本队列只暴露 try_push 失败信号）
 *
 * 设计：经典 single-producer / single-consumer 环形缓冲，head/tail 各占一个 cache line。
 * 容量必须是 2 的幂（取模优化）。
 */

#ifndef MC_PLAYER_PAL_SPSC_QUEUE_H_
#define MC_PLAYER_PAL_SPSC_QUEUE_H_

#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

#include "pal/cache_line.h"

namespace mcp::pal {

template <typename T>
class SpscQueue {
    static_assert(std::is_nothrow_move_constructible_v<T> || std::is_nothrow_copy_constructible_v<T>,
                  "SpscQueue<T> requires nothrow movable or copyable element type");

public:
    explicit SpscQueue(std::size_t capacity_pow2)
        : capacity_{capacity_pow2}, mask_{capacity_pow2 - 1}, slots_(capacity_pow2) {
        assert(capacity_pow2 >= 2 && std::has_single_bit(capacity_pow2) &&
               "SpscQueue capacity must be a power of two and >= 2");
    }

    SpscQueue(const SpscQueue&)            = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    /// 仅生产者线程调用。
    template <typename U>
    bool try_push(U&& value) noexcept(std::is_nothrow_assignable_v<T&, U&&>) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = head + 1;
        if (next - tail_.load(std::memory_order_acquire) > capacity_) {
            return false;  // full
        }
        slots_[head & mask_] = std::forward<U>(value);
        head_.store(next, std::memory_order_release);
        return true;
    }

    /// 仅消费者线程调用。
    bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;  // empty
        }
        out = std::move(slots_[tail & mask_]);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// 仅消费者线程调用——丢最老一项。返回是否丢弃成功。
    bool try_drop_oldest() noexcept {
        T sink{};
        return try_pop(sink);
    }

    /// 任意线程调用，返回近似值（仅诊断用）。
    [[nodiscard]] std::size_t approx_size() const noexcept {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return head - tail;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    /// 本会话最高水位（性能量度规范 §3.4 `mc.queue.<name>.high_water_frames`）。
    /// 由生产者侧在 try_push 后手动 record；不在 hot path 自动更新（避免每次推都一次 RMW）。
    /// 调用方典型用法：
    ///   if (q.try_push(x)) q.record_water_mark();
    void record_water_mark() noexcept {
        const std::size_t cur = approx_size();
        std::size_t prev = high_water_.load(std::memory_order_relaxed);
        while (cur > prev &&
               !high_water_.compare_exchange_weak(prev, cur, std::memory_order_relaxed)) {
            // retry
        }
    }

    [[nodiscard]] std::size_t high_water() const noexcept {
        return high_water_.load(std::memory_order_relaxed);
    }

    /// 累计"丢最老"次数（性能量度规范 §3.4 `mc.queue.<name>.drop_oldest_count`）。
    /// 调用方应在执行 try_drop_oldest 后调 inc_drop_oldest（满时丢弃由调用方决定，本类只暴露计数器）。
    void inc_drop_oldest() noexcept {
        drop_oldest_count_.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t drop_oldest_count() const noexcept {
        return drop_oldest_count_.load(std::memory_order_relaxed);
    }

private:
    const std::size_t capacity_;
    const std::size_t mask_;
    std::vector<T>    slots_;

    alignas(kCacheLineSize) std::atomic<std::size_t> head_{0};
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_{0};
    // metric counter（独立 cache line，避免与 hot path head/tail false-sharing）。
    alignas(kCacheLineSize) std::atomic<std::size_t> high_water_{0};
    std::atomic<uint64_t>                            drop_oldest_count_{0};
    // 用末尾 padding 占据剩余 cache line，确保下一个 SpscQueue / 邻接结构独立。
    char pad_tail_[kCacheLineSize - sizeof(std::atomic<std::size_t>) -
                   sizeof(std::atomic<uint64_t>)]{};
};

}  // namespace mcp::pal

#endif  // MC_PLAYER_PAL_SPSC_QUEUE_H_
