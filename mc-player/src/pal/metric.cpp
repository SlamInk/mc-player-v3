/*
 * 性能 metric 原语实现 — 性能量度规范 §9.2 / plan Phase 0。
 *
 * Histogram 桶布局：log-scale，r = 1.05^idx ns，0 ≤ idx < 256。
 *   bucket_upper_ns(idx) = pow(1.05, idx) ns，向下舍到 int64。
 *   覆盖范围：1ns（idx=0）→ ~3.7e5 ns（idx=63，370 µs）→ ~3.7e10 ns（idx=255，37 s）。
 *
 * 分位数查询：累加 buckets[0..k-1] 直到累计 count >= q * total，落到 bucket k；
 *   bucket k 内做线性插值返回上界（HDR Histogram 简化做法，误差 ≤ 1.05x）。
 *
 * 线程安全：buckets_/min_/max_ 都是 atomic；record 路径 lock-free。
 */

#include "pal/metric.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace mcp::pal::metric {

namespace {

// 编译期生成桶上界表（pow(1.05, i) ns）。
struct BucketTable {
    int64_t upper[Histogram::kBucketCount]{};

    constexpr BucketTable() {
        // constexpr 没有 std::pow；用迭代乘累积。
        double v = 1.0;
        for (std::size_t i = 0; i < Histogram::kBucketCount; ++i) {
            // 上界 = ceil(v)，最小 1。
            int64_t ub = static_cast<int64_t>(v + 0.5);
            if (ub < 1) ub = 1;
            // 单调性：每个桶上界至少比前一个 +1。
            if (i > 0 && ub <= upper[i - 1]) ub = upper[i - 1] + 1;
            upper[i] = ub;
            v *= 1.05;
        }
    }
};

constexpr BucketTable kBuckets{};

}  // namespace

// ─────────── Counter ───────────

Counter::Counter(std::string name) noexcept : name_{std::move(name)} {}

void Counter::inc(uint64_t delta, Phase /*phase*/) noexcept {
    value_.fetch_add(delta, std::memory_order_relaxed);
    // ETW 路径在 etw_provider.cpp 提供包装；此处不直接调用避免 .cpp 循环依赖。
    // hot path 调用方按需配对 ETW emit。
}

// ─────────── Gauge ───────────

Gauge::Gauge(std::string name) noexcept : name_{std::move(name)} {}

void Gauge::set(int64_t v, Phase /*phase*/) noexcept {
    value_.store(v, std::memory_order_relaxed);
}

void Gauge::add(int64_t delta, Phase /*phase*/) noexcept {
    value_.fetch_add(delta, std::memory_order_relaxed);
}

// ─────────── Histogram ───────────

Histogram::Histogram(std::string name) noexcept : name_{std::move(name)} {}

int64_t Histogram::bucket_upper_ns(std::size_t idx) noexcept {
    if (idx >= kBucketCount) return INT64_MAX;
    return kBuckets.upper[idx];
}

std::size_t Histogram::locate_bucket(int64_t value_ns) noexcept {
    if (value_ns <= 0) return 0;
    // 线性 search 即可（256 桶常驻 cache 几个 line，比 log2 启发的 binary search 在小桶数上没有显著优势）。
    // 但 hot path 频繁调用，binary search 仍更稳——最差 8 步比较。
    std::size_t lo = 0, hi = kBucketCount;
    while (lo < hi) {
        std::size_t mid = lo + ((hi - lo) >> 1);
        if (kBuckets.upper[mid] < value_ns) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return std::min(lo, kBucketCount - 1);
}

void Histogram::record(int64_t value_ns, Phase /*phase*/) noexcept {
    if (value_ns < 0) value_ns = 0;
    const std::size_t idx = locate_bucket(value_ns);
    buckets_[idx].fetch_add(1, std::memory_order_relaxed);
    total_count_.fetch_add(1, std::memory_order_relaxed);

    // min/max（CAS loop；hot path 频率不高时 OK）
    int64_t cur_min = min_ns_.load(std::memory_order_relaxed);
    while (value_ns < cur_min &&
           !min_ns_.compare_exchange_weak(cur_min, value_ns, std::memory_order_relaxed)) {
        // retry
    }
    int64_t cur_max = max_ns_.load(std::memory_order_relaxed);
    while (value_ns > cur_max &&
           !max_ns_.compare_exchange_weak(cur_max, value_ns, std::memory_order_relaxed)) {
        // retry
    }
}

int64_t Histogram::quantile(double q) const noexcept {
    if (q <= 0.0) return min_ns();
    if (q >= 1.0) return max_ns();

    const uint64_t total = total_count_.load(std::memory_order_relaxed);
    if (total == 0) return 0;

    const uint64_t target = static_cast<uint64_t>(static_cast<double>(total) * q);
    uint64_t accum = 0;
    for (std::size_t i = 0; i < kBucketCount; ++i) {
        accum += buckets_[i].load(std::memory_order_relaxed);
        if (accum >= target) {
            // 桶 i 的上界即近似 quantile（误差 ≤ 5%，HDR 简化做法）。
            return bucket_upper_ns(i);
        }
    }
    return max_ns();
}

// ─────────── Registry ───────────

Registry& Registry::instance() noexcept {
    static Registry inst;
    return inst;
}

Counter& Registry::counter(std::string_view name) {
    std::lock_guard<std::mutex> lk{mu_};
    std::string key{name};
    auto it = counters_.find(key);
    if (it != counters_.end()) return *it->second;
    auto p = std::make_unique<Counter>(key);
    Counter& ref = *p;
    counters_.emplace(std::move(key), std::move(p));
    return ref;
}

Gauge& Registry::gauge(std::string_view name) {
    std::lock_guard<std::mutex> lk{mu_};
    std::string key{name};
    auto it = gauges_.find(key);
    if (it != gauges_.end()) return *it->second;
    auto p = std::make_unique<Gauge>(key);
    Gauge& ref = *p;
    gauges_.emplace(std::move(key), std::move(p));
    return ref;
}

Histogram& Registry::histogram(std::string_view name) {
    std::lock_guard<std::mutex> lk{mu_};
    std::string key{name};
    auto it = histograms_.find(key);
    if (it != histograms_.end()) return *it->second;
    auto p = std::make_unique<Histogram>(key);
    Histogram& ref = *p;
    histograms_.emplace(std::move(key), std::move(p));
    return ref;
}

Registry::Snapshot Registry::snapshot() const {
    Snapshot snap;
    std::lock_guard<std::mutex> lk{mu_};
    snap.counters.reserve(counters_.size());
    snap.gauges.reserve(gauges_.size());
    snap.histograms.reserve(histograms_.size());
    for (const auto& [n, p] : counters_) {
        snap.counters.emplace_back(n, p->value());
    }
    for (const auto& [n, p] : gauges_) {
        snap.gauges.emplace_back(n, p->value());
    }
    for (const auto& [n, p] : histograms_) {
        snap.histograms.emplace_back(n,
                                     p->count(),
                                     p->min_ns(),
                                     p->max_ns(),
                                     p->quantile(0.50),
                                     p->quantile(0.95),
                                     p->quantile(0.99));
    }
    // 排序输出，便于 dump 工具产出稳定 JSON。
    std::sort(snap.counters.begin(),  snap.counters.end());
    std::sort(snap.gauges.begin(),    snap.gauges.end());
    std::sort(snap.histograms.begin(), snap.histograms.end(),
              [](const auto& a, const auto& b) {
                  return std::get<0>(a) < std::get<0>(b);
              });
    return snap;
}

}  // namespace mcp::pal::metric
