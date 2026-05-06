#include "probe/network_probe.h"

#include <atomic>

namespace mcp::probe {

struct NetworkProbe::Impl {
    std::atomic<int64_t>  rtt_p50_ns{0};
    std::atomic<int64_t>  rtt_p95_ns{0};
    std::atomic<int64_t>  rtt_p99_ns{0};
    std::atomic<uint64_t> packet_count{0};
    std::atomic<uint64_t> loss_count{0};
    std::atomic<bool>     first_gop_done{false};
};

NetworkProbe::NetworkProbe() : impl_{std::make_unique<Impl>()} {}
NetworkProbe::~NetworkProbe() = default;

void NetworkProbe::record_rtt_sample_ns(int64_t rtt_ns) noexcept {
    // Phase 9.0 主线: HDR Histogram 采样 + 1Hz 输出 p50/p95/p99。
    // 当前简化:仅维护最新值,不算分位数。
    impl_->rtt_p95_ns.store(rtt_ns, std::memory_order_relaxed);
}

void NetworkProbe::record_packet(int64_t /*arrival_ns*/, uint16_t /*seq*/,
                                   uint32_t /*rtp_ts*/) noexcept {
    impl_->packet_count.fetch_add(1, std::memory_order_relaxed);
    // Phase 9.0 主线: 滑窗 IAT jitter + seq gap loss 统计。
}

NetworkSnapshot NetworkProbe::snapshot() const noexcept {
    NetworkSnapshot out;
    out.rtt_p95_ms = static_cast<uint32_t>(
        impl_->rtt_p95_ns.load(std::memory_order_relaxed) / 1'000'000);
    const uint64_t pkts = impl_->packet_count.load(std::memory_order_relaxed);
    const uint64_t lost = impl_->loss_count.load(std::memory_order_relaxed);
    out.loss_rate_short = pkts ? static_cast<double>(lost) / static_cast<double>(pkts) : 0.0;
    out.complete        = impl_->first_gop_done.load(std::memory_order_relaxed);
    // link_kind 推断按 ADD §7.5.2 阈值表:
    //   loss<0.001 + rtt<5ms       → LAN_SWITCHED
    //   loss<0.01  + rtt<20ms      → LAN_WIFI
    //   loss<0.03  + rtt<60ms      → WAN_BROADBAND
    //   else                        → WAN_LOSSY
    // Phase 9.0 主线实装,当前默认 unknown。
    out.link_kind = MC_LINK_KIND_UNKNOWN;
    return out;
}

bool NetworkProbe::ready() const noexcept {
    return impl_->first_gop_done.load(std::memory_order_relaxed);
}

}  // namespace mcp::probe
