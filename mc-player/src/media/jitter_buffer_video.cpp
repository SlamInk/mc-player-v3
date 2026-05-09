#include "media/jitter_buffer_video.h"

#include <array>
#include <atomic>

#include "pal/clock.h"
#include "pal/log.h"
#include "pal/metric.h"
#include "transport/rtp_packet.h"

namespace mcp::media {

namespace {

// Ring buffer 容量:覆盖典型 LAN/IPC 场景的最大乱序+待 NACK window。
//   30 fps 视频流,每帧 ~20 RTP(1280x720 IDR),~600 packets/s。
//   dwell=30ms 内最多 ~18 个包待 emit;reorder window 取 4 倍冗余。
//   实际取 256(~430ms @ 600pps)以应对偶发 burst loss + 留 NACK 重传时间。
//   power-of-2 让 (seq % CAP) 可以化为 (seq & MASK) 微优化。
constexpr int kRingCap   = 256;
constexpr int kRingMask  = kRingCap - 1;
static_assert((kRingCap & kRingMask) == 0, "kRingCap must be power of 2");

// 默认 dwell 30ms(LAN-IPC reorder 实测 95% 在 5-15ms,30ms 留 2x 余量)。
// 远端互联网链路可能要 50-80ms;由 controller/preset 后续 reload 调。
constexpr int64_t kDefaultDwellUs       = 30'000;
// NACK 重发限频:同 pid 最少 50ms 一次,防 NACK 风暴。
constexpr int64_t kDefaultNackIntervalUs = 50'000;

}  // namespace

struct JitterBufferVideo::Impl {
    EmitFn emit;
    NackFn nack;
    LossFn loss;

    int64_t dwell_us             = kDefaultDwellUs;
    int64_t nack_min_interval_us = kDefaultNackIntervalUs;

    // 32-bit extended seq:处理 16-bit wrap-around(每 13.6h @ 90kHz)。
    // bootstrap 时取首包 16-bit seq 作为低 16 位起点。
    bool     seen_first        = false;
    uint32_t next_expected_seq = 0;     // 下一个要 emit 的 ext-seq
    uint32_t highest_seen_seq  = 0;     // 已 push 进 buffer 的最大 ext-seq

    struct Slot {
        bool                  filled         = false;
        bool                  nacked         = false;
        int64_t               arrival_qpc_ns = 0;
        int64_t               first_nack_ns  = 0;
        transport::RtpDatagram dg;
    };
    std::array<Slot, kRingCap> ring{};

    // Stats(atomic 让 controller 1Hz tick 跨线程读)
    std::atomic<uint64_t> emits{0};
    std::atomic<uint64_t> late_drops{0};
    std::atomic<uint64_t> duplicate_drops{0};
    std::atomic<uint64_t> expired_gaps{0};
    std::atomic<uint64_t> nacks_sent{0};
    std::atomic<uint64_t> reorder_events{0};
    std::atomic<uint32_t> pending_count{0};
    std::atomic<uint32_t> max_pending_seen{0};

    pal::metric::Counter&   m_emit       = pal::metric::Registry::instance().counter("mc.jitter.emit_count");
    pal::metric::Counter&   m_late       = pal::metric::Registry::instance().counter("mc.jitter.late_drop_count");
    pal::metric::Counter&   m_dup        = pal::metric::Registry::instance().counter("mc.jitter.dup_drop_count");
    pal::metric::Counter&   m_expired    = pal::metric::Registry::instance().counter("mc.jitter.expired_gap_count");
    pal::metric::Counter&   m_nack       = pal::metric::Registry::instance().counter("mc.jitter.nack_sent_count");
    pal::metric::Counter&   m_reorder    = pal::metric::Registry::instance().counter("mc.jitter.reorder_event_count");
    pal::metric::Gauge&     m_pending    = pal::metric::Registry::instance().gauge("mc.jitter.pending_count");
    pal::metric::Histogram& m_dwell_hist = pal::metric::Registry::instance().histogram("mc.stage.jitter_dwell_ns");

    // 把 16-bit RTP seq 提升到 32-bit extended seq。
    // 规则:与 highest_seen_seq 的低 16 位比较 — 差 |Δ|<32768 视为同一周期(更近的
    // 历史/未来),|Δ|≥32768 视为已 wrap。
    // bootstrap 时:next_expected_seq 高 16 位 = 0,低 16 位 = 首包 seq。
    [[nodiscard]] uint32_t to_ext_seq(uint16_t seq16) const noexcept {
        const uint16_t ref16 = static_cast<uint16_t>(highest_seen_seq & 0xFFFFu);
        const int16_t  delta = static_cast<int16_t>(seq16 - ref16);
        return static_cast<uint32_t>(static_cast<int64_t>(highest_seen_seq) + delta);
    }

    void update_pending() noexcept {
        const uint32_t cur = pending_count.load(std::memory_order_relaxed);
        m_pending.set(static_cast<int64_t>(cur));
        const uint32_t prev_max = max_pending_seen.load(std::memory_order_relaxed);
        if (cur > prev_max) {
            max_pending_seen.compare_exchange_strong(
                const_cast<uint32_t&>(prev_max), cur, std::memory_order_relaxed);
        }
    }

    // 核心入口:接 RTP datagram。
    void on_rtp(const transport::RtpDatagram& dg) noexcept {
        if (dg.bytes.size() < 12) return;     // 非法 RTP

        // 解析 seq(big-endian,RTP header byte 2-3)
        const uint16_t seq16 = static_cast<uint16_t>(
            (static_cast<uint16_t>(dg.bytes[2]) << 8) | dg.bytes[3]);

        const int64_t now_ns = (dg.arrival_qpc_ns != 0)
                                ? dg.arrival_qpc_ns
                                : pal::Clock::now_ns();

        if (!seen_first) {
            seen_first        = true;
            next_expected_seq = seq16;        // 高 16 位 = 0
            highest_seen_seq  = seq16;
            // 直接 emit 第一个包,推进 next_expected;dwell = 0(首包不滞留)。
            m_dwell_hist.record(0);
            if (emit) emit(dg);
            emits.fetch_add(1, std::memory_order_relaxed);
            m_emit.inc();
            ++next_expected_seq;
            return;
        }

        const uint32_t ext = to_ext_seq(seq16);

        // 太旧:seq < next_expected → late or duplicate(NACK 重传到达 emit 后)
        if (ext < next_expected_seq) {
            // 区分 late(seq 远早于 next_expected)和 duplicate(刚 emit 完 NACK 重传)
            // 简化:统一标 late_drops + 单独计 duplicate(seq == next_expected_seq - 1
            // 的 emit 已经发生)。生产场景两类都 drop。
            const uint32_t back = next_expected_seq - ext;
            if (back <= 32) {
                duplicate_drops.fetch_add(1, std::memory_order_relaxed);
                m_dup.inc();
            } else {
                late_drops.fetch_add(1, std::memory_order_relaxed);
                m_late.inc();
            }
            return;
        }

        // 太新:超出 ring 容量。可能 long burst loss + 后续大量包累积,
        // 或 wrap-around 被误判。直接强制推进 next_expected 跳过 gap 释放空间。
        const uint32_t ahead = ext - next_expected_seq;
        if (ahead >= kRingCap) {
            // Force-skip [next_expected_seq, ext - kRingCap + 1) 让 ring 空位出来。
            // 报丢失计数。
            const uint32_t skip_count = ahead - (kRingCap - 1);
            for (uint32_t i = 0; i < skip_count; ++i) {
                expired_gaps.fetch_add(1, std::memory_order_relaxed);
                m_expired.inc();
                if (loss) {
                    loss(static_cast<uint16_t>(next_expected_seq & 0xFFFFu), 1);
                }
                ++next_expected_seq;
            }
        }

        // 入桶
        Slot& s = ring[ext & kRingMask];
        if (s.filled) {
            // 槽里已经有数据 — 检查是同 ext_seq(duplicate)还是新覆盖(理论不应发生)。
            // 由于 ring_cap=256 + 上面 force-skip 保证 next_expected_seq..next+CAP-1 区间,
            // 旧 seq 已被 emit/skip 清空;这里若 filled 仅可能是 NACK 重传 duplicate。
            duplicate_drops.fetch_add(1, std::memory_order_relaxed);
            m_dup.inc();
            return;
        }
        s.filled         = true;
        s.nacked         = false;
        s.arrival_qpc_ns = now_ns;
        s.first_nack_ns  = 0;
        s.dg             = dg;
        pending_count.fetch_add(1, std::memory_order_relaxed);

        if (ext < highest_seen_seq) {
            reorder_events.fetch_add(1, std::memory_order_relaxed);
            m_reorder.inc();
        } else {
            highest_seen_seq = ext;
        }

        // drain consecutive
        drain();

        // gap detection 触发 NACK
        detect_gaps_and_nack(now_ns);

        // 入包同时检查 dwell 超时(适合稀疏流稳态)
        sweep_expired(now_ns);

        update_pending();
    }

    // 从 next_expected_seq 起连续 emit
    void drain() noexcept {
        while (true) {
            Slot& s = ring[next_expected_seq & kRingMask];
            if (!s.filled) break;
            // 记 jitter dwell:从 arrival 到 emit 的滞留 ns(in-order/低 dwell ≈ 0,
            // reorder/loss 重传补回的会显著抬高 p95)。
            const int64_t now_emit_ns = pal::Clock::now_ns();
            const int64_t dwell_ns    = (s.arrival_qpc_ns > 0)
                                         ? (now_emit_ns - s.arrival_qpc_ns)
                                         : 0;
            if (dwell_ns >= 0) m_dwell_hist.record(dwell_ns);
            if (emit) emit(s.dg);
            emits.fetch_add(1, std::memory_order_relaxed);
            m_emit.inc();
            // 释放槽
            s.filled         = false;
            s.nacked         = false;
            s.arrival_qpc_ns = 0;
            s.first_nack_ns  = 0;
            s.dg             = transport::RtpDatagram{};
            pending_count.fetch_sub(1, std::memory_order_relaxed);
            ++next_expected_seq;
        }
    }

    // dwell 超时 sweep:next_expected slot 长期空 → 跳过让 drain 继续。
    void sweep_expired(int64_t now_ns) noexcept {
        // 只在 next_expected_seq slot 空 + 后面有 filled slot 等待时检查。
        // 用「最早 filled slot 的 arrival 时间」作为 reference:超过 dwell 即放弃 next_expected。
        while (next_expected_seq < highest_seen_seq + 1) {
            const Slot& cur = ring[next_expected_seq & kRingMask];
            if (cur.filled) break;     // 不需 sweep,drain 会处理

            // 找下一个 filled slot 的 arrival 时间
            int64_t earliest_pending_ns = 0;
            for (uint32_t s = next_expected_seq + 1; s <= highest_seen_seq; ++s) {
                const Slot& q = ring[s & kRingMask];
                if (q.filled) {
                    if (earliest_pending_ns == 0 || q.arrival_qpc_ns < earliest_pending_ns) {
                        earliest_pending_ns = q.arrival_qpc_ns;
                    }
                }
            }
            if (earliest_pending_ns == 0) break;     // 后面没 filled,等 next 包

            const int64_t dwell_ns = (now_ns - earliest_pending_ns);
            if (dwell_ns < dwell_us * 1000) break;     // 还在 reorder window 内

            // 跳过 next_expected_seq
            expired_gaps.fetch_add(1, std::memory_order_relaxed);
            m_expired.inc();
            if (loss) {
                loss(static_cast<uint16_t>(next_expected_seq & 0xFFFFu), 1);
            }
            ++next_expected_seq;
            // 然后 drain 一次:下一个可能就是 filled
            drain();
        }
    }

    // 扫 next_expected..highest_seen 中空槽,触发 NACK(限频)。
    // 收集连续/邻近的 missing seq 用 PID+BLP 一次性 NACK 16 个。
    void detect_gaps_and_nack(int64_t now_ns) noexcept {
        if (!nack) return;
        if (next_expected_seq >= highest_seen_seq) return;

        uint32_t s = next_expected_seq;
        while (s <= highest_seen_seq) {
            // 找首个 missing
            while (s <= highest_seen_seq && ring[s & kRingMask].filled) ++s;
            if (s > highest_seen_seq) break;

            const uint32_t pid = s;
            uint16_t blp = 0;
            // BLP bit i = (pid + 1 + i) 是否丢失;扫后 16 个
            for (int i = 0; i < 16 && (pid + 1u + static_cast<uint32_t>(i)) <= highest_seen_seq; ++i) {
                const uint32_t k = pid + 1u + static_cast<uint32_t>(i);
                if (!ring[k & kRingMask].filled) {
                    blp |= static_cast<uint16_t>(1u << i);
                }
            }

            // 限频 — 每个 pid slot 记录 first_nack_ns,nack_min_interval 内不重发。
            Slot& slot = ring[pid & kRingMask];
            const bool need_send = !slot.nacked
                || (now_ns - slot.first_nack_ns) >= nack_min_interval_us * 1000;
            if (need_send) {
                slot.nacked        = true;
                slot.first_nack_ns = now_ns;
                nack(static_cast<uint16_t>(pid & 0xFFFFu), blp);
                nacks_sent.fetch_add(1, std::memory_order_relaxed);
                m_nack.inc();
            }

            // 跳过这一组(pid + BLP 覆盖的 16 个),继续扫后续 missing
            s = pid + 17u;
        }
    }
};

JitterBufferVideo::JitterBufferVideo(EmitFn emit, NackFn nack, LossFn loss) noexcept
    : impl_{std::make_unique<Impl>()} {
    impl_->emit = std::move(emit);
    impl_->nack = std::move(nack);
    impl_->loss = std::move(loss);
}

JitterBufferVideo::~JitterBufferVideo() noexcept = default;

void JitterBufferVideo::configure(int64_t dwell_us, int64_t nack_min_interval_us) noexcept {
    if (dwell_us > 0) impl_->dwell_us = dwell_us;
    if (nack_min_interval_us > 0) impl_->nack_min_interval_us = nack_min_interval_us;
    MCP_LOGF(pal::LogLevel::info,
             "JitterBufferVideo: dwell=%lldus nack_interval=%lldus ring_cap=%d",
             static_cast<long long>(impl_->dwell_us),
             static_cast<long long>(impl_->nack_min_interval_us),
             kRingCap);
}

void JitterBufferVideo::on_rtp(const transport::RtpDatagram& dg) noexcept {
    impl_->on_rtp(dg);
}

void JitterBufferVideo::tick(int64_t now_qpc_ns) noexcept {
    impl_->sweep_expired(now_qpc_ns);
    impl_->detect_gaps_and_nack(now_qpc_ns);
    impl_->update_pending();
}

void JitterBufferVideo::reset() noexcept {
    for (auto& s : impl_->ring) {
        s.filled         = false;
        s.nacked         = false;
        s.arrival_qpc_ns = 0;
        s.first_nack_ns  = 0;
        s.dg             = transport::RtpDatagram{};
    }
    impl_->seen_first        = false;
    impl_->next_expected_seq = 0;
    impl_->highest_seen_seq  = 0;
    impl_->pending_count.store(0, std::memory_order_relaxed);
    impl_->m_pending.set(0);
    // 不清 emits / late_drops 等累计计数(供观测)。
}

JitterBufferVideo::Stats JitterBufferVideo::stats() const noexcept {
    Stats s;
    s.emits            = impl_->emits.load(std::memory_order_relaxed);
    s.late_drops       = impl_->late_drops.load(std::memory_order_relaxed);
    s.duplicate_drops  = impl_->duplicate_drops.load(std::memory_order_relaxed);
    s.expired_gaps     = impl_->expired_gaps.load(std::memory_order_relaxed);
    s.nacks_sent       = impl_->nacks_sent.load(std::memory_order_relaxed);
    s.reorder_events   = impl_->reorder_events.load(std::memory_order_relaxed);
    s.pending_count    = impl_->pending_count.load(std::memory_order_relaxed);
    s.max_pending_seen = impl_->max_pending_seen.load(std::memory_order_relaxed);
    return s;
}

}  // namespace mcp::media
