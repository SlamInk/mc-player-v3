/*
 * 性能 metric 原语 — 性能量度规范 §9.2 / plan Phase 0 §0.1。
 *
 * 四类原语（按性能量度规范 §9.2）：
 *   - Counter：单调递增；atomic<uint64_t>。
 *   - Gauge：当前值；atomic<int64_t>（允许负值，如 av_offset_ns）。
 *   - Histogram：分布 + 分位数；HDR-style log buckets，固定 ~250 桶覆盖 1ns ~ 10s。
 *   - Timer：Histogram 的语义子类 + ScopedTimer RAII；统一用 QPC ns。
 *
 * 全局 Registry：按 metric 名 lookup 共享实例（避免每个 hot path 复创建）。
 * 命名规范：`mc.<domain>.<metric>[_<unit_suffix>]`（性能量度规范 §9.1）。
 *
 * Phase 标签（性能量度规范 §1.2）：每条 record 附带 `phase` ∈ {cold_start, warm_steady, error_recovery}；
 * 通过 ETW Provider 的 keyword 字段透传，实现层只在 record 时入参，不分桶存储（避免桶数 ×3）。
 *
 * 与 ETW 关系：record 路径同时调用 ETW Provider event；ETW 是源头数据，metric 内存计数是 1Hz 聚合的快照
 * 来源（性能量度规范 §9.3 上报通道表）。
 */

#ifndef MC_PLAYER_PAL_METRIC_H_
#define MC_PLAYER_PAL_METRIC_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pal/clock.h"

namespace mcp::pal::metric {

/// Phase 标签（性能量度规范 §1.2）。
enum class Phase : uint8_t {
    ColdStart      = 0,
    WarmSteady     = 1,
    ErrorRecovery  = 2,
};

/// Counter：单调递增计数器。
class Counter {
public:
    explicit Counter(std::string name) noexcept;

    void inc(uint64_t delta = 1, Phase phase = Phase::WarmSteady) noexcept;

    [[nodiscard]] uint64_t value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::string_view name() const noexcept { return name_; }

private:
    std::string             name_;
    std::atomic<uint64_t>   value_{0};
};

/// Gauge：当前值，允许负数。
class Gauge {
public:
    explicit Gauge(std::string name) noexcept;

    void set(int64_t v, Phase phase = Phase::WarmSteady) noexcept;
    void add(int64_t delta, Phase phase = Phase::WarmSteady) noexcept;

    [[nodiscard]] int64_t value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::string_view name() const noexcept { return name_; }

private:
    std::string             name_;
    std::atomic<int64_t>    value_{0};
};

/// Histogram：HDR-style log buckets，覆盖 1ns ~ ~10s。
///
/// 实现：每桶宽度 1.05× 前一桶，约 250 桶覆盖 1e0 ~ 1e10 ns。
/// 桶边界用 `bucket_upper_ns(idx)` 计算；record 时 binary search 落桶。
/// 分位数 query 时累加桶 count，线性插值近似（HDR 标准做法）。
class Histogram {
public:
    static constexpr std::size_t kBucketCount = 256;

    explicit Histogram(std::string name) noexcept;

    /// 记录一次观测值（ns）。线程安全。
    void record(int64_t value_ns, Phase phase = Phase::WarmSteady) noexcept;

    /// 查询分位数（quantile ∈ [0, 1]）。**仅诊断用，调用方不在 hot path**。
    [[nodiscard]] int64_t quantile(double q) const noexcept;

    [[nodiscard]] uint64_t count() const noexcept {
        return total_count_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] int64_t min_ns() const noexcept {
        return min_ns_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] int64_t max_ns() const noexcept {
        return max_ns_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::string_view name() const noexcept { return name_; }

    /// 桶上界（ns）。idx ∈ [0, kBucketCount)。
    static int64_t bucket_upper_ns(std::size_t idx) noexcept;

    /// 落桶（binary search）。
    static std::size_t locate_bucket(int64_t value_ns) noexcept;

private:
    std::string             name_;
    std::atomic<uint64_t>   buckets_[kBucketCount]{};
    std::atomic<uint64_t>   total_count_{0};
    std::atomic<int64_t>    min_ns_{INT64_MAX};
    std::atomic<int64_t>    max_ns_{0};
};

/// Timer：Histogram 的语义子类。所有 `_ns` 时延 metric 用此。
using Timer = Histogram;

/// RAII 自动 record 时延：构造记 t0，析构 record(qpc_now - t0)。
class ScopedTimer {
public:
    ScopedTimer(Timer& timer, Phase phase = Phase::WarmSteady) noexcept
        : timer_{timer}, phase_{phase}, t0_ns_{Clock::now_ns()} {}

    ~ScopedTimer() noexcept {
        if (!disarmed_) {
            timer_.record(Clock::now_ns() - t0_ns_, phase_);
        }
    }

    ScopedTimer(const ScopedTimer&)            = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

    /// 不再 record（如阶段提前 abort，或转交其它 Timer）。
    void disarm() noexcept { disarmed_ = true; }

    /// 提前结束并 record；调用后析构不再触发。
    void stop() noexcept {
        if (!disarmed_) {
            timer_.record(Clock::now_ns() - t0_ns_, phase_);
            disarmed_ = true;
        }
    }

private:
    Timer&  timer_;
    Phase   phase_;
    int64_t t0_ns_;
    bool    disarmed_{false};
};

/// 全局 Registry：按 metric 名 lookup 共享实例。
class Registry {
public:
    static Registry& instance() noexcept;

    /// Get-or-create：相同名返回同实例。线程安全。
    Counter&    counter(std::string_view name);
    Gauge&      gauge(std::string_view name);
    Histogram&  histogram(std::string_view name);
    Timer&      timer(std::string_view name) { return histogram(name); }

    /// 用于 stats 聚合：枚举所有 metric。**仅 1Hz 聚合或离线 dump 调用**。
    struct Snapshot {
        std::vector<std::pair<std::string, uint64_t>>           counters;
        std::vector<std::pair<std::string, int64_t>>            gauges;
        std::vector<std::tuple<std::string, uint64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t>> histograms;
        // tuple: name, count, min, max, p50, p95, p99
    };
    [[nodiscard]] Snapshot snapshot() const;

private:
    Registry() noexcept = default;

    mutable std::mutex                                      mu_;
    std::unordered_map<std::string, std::unique_ptr<Counter>>   counters_;
    std::unordered_map<std::string, std::unique_ptr<Gauge>>     gauges_;
    std::unordered_map<std::string, std::unique_ptr<Histogram>> histograms_;
};

}  // namespace mcp::pal::metric

#endif  // MC_PLAYER_PAL_METRIC_H_
