#include "media/video_clock.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>

#include "pal/clock.h"
#include "pal/log.h"
#include "pal/metric.h"

namespace mcp::media {

namespace {

inline pal::metric::Counter& m_present() {
    return pal::metric::Registry::instance().counter("mc.video_clock.present_count");
}
inline pal::metric::Counter& m_coeff_reset() {
    return pal::metric::Registry::instance().counter("mc.video_clock.coeff_reset_count");
}
inline pal::metric::Counter& m_discontinuity() {
    return pal::metric::Registry::instance().counter("mc.video_clock.discontinuity_count");
}
inline pal::metric::Counter& m_wait_interrupted() {
    return pal::metric::Registry::instance().counter("mc.video_clock.wait_interrupted_count");
}
inline pal::metric::Histogram& h_drift() {
    return pal::metric::Registry::instance().histogram("mc.video_clock.drift_abs_ns");
}
inline pal::metric::Histogram& h_coeff_x1000() {
    // coeff 是 double ≈ 1.0;histogram 期望 ns-范围整数,*1000 让 1.0→1000、1.001→1001 可分桶。
    return pal::metric::Registry::instance().histogram("mc.video_clock.coeff_x1000");
}

}  // namespace

// ─── Average(对齐 VLC AvgUpdate / AvgGet / AvgResetAndFill)──────────────

void Average::update(double v) noexcept {
    // VLC AvgUpdate:
    //   weight   = (count < range) ? count++ : range - 1
    //   divisor  = (count < range) ? count   : range
    //   value    = (weight * value + new_value) / divisor
    int weight, divisor;
    if (count_ < kRange) {
        weight  = count_++;
        divisor = count_;
    } else {
        weight  = kRange - 1;
        divisor = kRange;
    }
    value_ = (weight * value_ + v) / static_cast<double>(divisor);
}

void Average::reset_and_fill(double v) noexcept {
    value_ = v;
    count_ = kRange;     // 立即满 — 后续 update 等价完整滑窗 EMA
}

// ─── VideoClock ─────────────────────────────────────────────────────────

VideoClock::VideoClock() noexcept = default;
VideoClock::~VideoClock() noexcept {
    request_stop();      // 析构前唤醒可能阻塞的 wait_until
}

bool VideoClock::has_anchor() const noexcept {
    std::scoped_lock lk{mu_};
    return ctx_.start_pts_us != INT64_MIN;
}

void VideoClock::master_update(int64_t arrival_qpc_ns,
                                int64_t pts_us,
                                double rate) noexcept {
    std::scoped_lock lk{mu_};

    // Bootstrap — 第一次 master_update。VLC vlc_clock_main_Update 首次行为:
    //   first_pcr.system = system_now;first_pcr.stream = ts;coeff=1, offset=0;
    if (ctx_.start_pts_us == INT64_MIN) {
        ctx_.start_pts_us = pts_us;
        ctx_.start_qpc_ns = arrival_qpc_ns;
        ctx_.last_pts_us  = pts_us;
        ctx_.last_qpc_ns  = arrival_qpc_ns;
        ctx_.coeff        = 1.0;
        ctx_.rate         = rate;
        ctx_.offset_ns    = 0;
        coeff_avg_.reset_and_fill(1.0);
        stat_coeff_instant_.store(1.0, std::memory_order_relaxed);
        MCP_LOGF(pal::LogLevel::info,
                 "VideoClock: bootstrap pts=%lld us → arrival_qpc=%lld ns rate=%.3f",
                 static_cast<long long>(pts_us),
                 static_cast<long long>(arrival_qpc_ns), rate);
        stat_present_.fetch_add(1, std::memory_order_relaxed);
        m_present().inc();
        return;
    }

    const int64_t pts_delta_us = pts_us - ctx_.last_pts_us;

    // PTS 不连续阈值:200ms(对齐 kDiscontinuityUs)。30fps 正常 PTS delta 33-67ms,
    // 启动期 codec reorder buffer drain 偶现 +200-500ms 跳变,必须 catch 这种异常重锚,
    // 否则 deadline 远超 wall clock 让 T5 wait 半秒+,startup 期形成永久 backlog。
    // 真正 seek / wrap 也会通过此阈值触发(>200ms 通常意味着流非顺序帧)。
    constexpr int64_t kRealDiscontinuityUs = kDiscontinuityUs;     // 200ms
    if (std::abs(pts_delta_us) > kRealDiscontinuityUs) {
        ++ctx_.context_id;
        stat_discontinuity_.fetch_add(1, std::memory_order_relaxed);
        m_discontinuity().inc();
        MCP_LOGF(pal::LogLevel::warn,
                 "VideoClock: PTS discontinuity %lld→%lld (Δ=%lld us > 1s) "
                 "重锚 anchor (context_id=%u)",
                 static_cast<long long>(ctx_.last_pts_us),
                 static_cast<long long>(pts_us),
                 static_cast<long long>(pts_delta_us),
                 ctx_.context_id);
        // 重锚定 — 用当前 (arrival, pts) 作为新 anchor。max-monotonic 保证下次 anchor
        // 推进单调(避免 B 帧 PTS 短跳让 anchor 倒退)。
        ctx_.start_pts_us = pts_us;
        ctx_.start_qpc_ns = arrival_qpc_ns;
        ctx_.last_pts_us  = pts_us;
        ctx_.last_qpc_ns  = arrival_qpc_ns;
        ctx_.coeff        = 1.0;
        ctx_.offset_ns    = 0;
        ctx_.rate         = rate;
        coeff_avg_.reset_and_fill(1.0);
        stat_coeff_instant_.store(1.0, std::memory_order_relaxed);
        cv_.notify_all();
        stat_present_.fetch_add(1, std::memory_order_relaxed);
        m_present().inc();
        return;
    }

    // 正常路径 — Phase 2 简化版:固定 coeff=1.0 不动 EMA。
    //
    // 为何不用 VLC EMA?
    //   - VLC EMA 假设 master 时序源严格 wall-clock 单调(audio playback 时序 = wall clock,
    //     instant_coeff > 0 始终);
    //   - 我们用 frame.arrival_qpc_ns(RTP 第一包到达)+ frame.pts_us(display order PTS):
    //       * arrival 是 decode order(RTP 包按 decode 顺序到达);
    //       * pts 是 display order(codec reorder 后 emit 顺序);
    //       * B 帧 PTS < 前一 P 帧 PTS,但 B 帧 RTP 在 P 帧 RTP 之后到达 → instant_coeff
    //         在 frame-level 出现负值 / 大幅振荡 → EMA 永远不收敛。
    //   - 解决:固定 coeff=1.0,假设 source/wall clock 频率差忽略不计(典型 RTP 90kHz
    //     编码器与本地 QPC 频率差 < 100ppm,30 秒累积差 < 3ms,对客户端无感)。
    //   - 永远 anchor 在 first 帧,不重算 offset,deadline 严格按 source PTS 推进。
    //
    // 仅用 max-monotonic 维护 last 信息(给 stats 和 discontinuity 检测用),不影响 anchor。
    if (pts_us > ctx_.last_pts_us) {
        ctx_.last_pts_us = pts_us;
        ctx_.last_qpc_ns = arrival_qpc_ns;
    }
    stat_coeff_instant_.store(1.0, std::memory_order_relaxed);

    stat_present_.fetch_add(1, std::memory_order_relaxed);
    m_present().inc();

    // 周期 log:每 60 帧 + 头 6 帧 + reset 时,打 coeff/offset 给调参用。
    const uint64_t pn = stat_present_.load(std::memory_order_relaxed);
    if (pn <= 6 || pn % 60 == 0) {
        MCP_LOGF(pal::LogLevel::info,
                 "VideoClock[stat] #%llu coeff=%.6f instant=%.6f offset=%lld "
                 "reset=%llu discont=%llu wait_int=%llu",
                 static_cast<unsigned long long>(pn),
                 ctx_.coeff,
                 stat_coeff_instant_.load(std::memory_order_relaxed),
                 static_cast<long long>(ctx_.offset_ns),
                 static_cast<unsigned long long>(stat_coeff_reset_.load()),
                 static_cast<unsigned long long>(stat_discontinuity_.load()),
                 static_cast<unsigned long long>(stat_wait_interrupted_.load()));
    }
}

int64_t VideoClock::convert_to_system(int64_t pts_us,
                                       int64_t now_qpc_fallback,
                                       double /*rate*/) const noexcept {
    std::scoped_lock lk{mu_};
    if (ctx_.start_pts_us == INT64_MIN) {
        // 还没 bootstrap — return now 让上层立即 present。第一次 master_update 后建立
        // anchor,后续 PTS-paced 接管。
        return now_qpc_fallback;
    }
    // Phase 2 改用 last_present anchor(而非 first anchor)+ source PTS delta 推进:
    //   deadline_arrival = last_qpc + (pts - last_pts) * 1000 ns/us
    // 关键:anchor 用 last_present_qpc 而非 first_arrival_qpc。原因:
    //   first_arrival 是 RTP IDR 时刻,但 first frame 到 T5 处理已经 50+ms 后,
    //   所有后续 expected 远在 wall_now 未来 → wait 巨大 → backlog 永久。
    //   用 last_present_qpc 让 anchor 跟 T5 实际进度,deadline 与 actual 同步。
    //   每帧 master_update 时 advance last_qpc = arrival_qpc,
    //   convert_to_system 算 last_qpc + (pts - last_pts)*1000 = 最近一帧 + 单帧间距。
    const int64_t stream_offset_ns = (pts_us - ctx_.last_pts_us) * 1000;
    return ctx_.last_qpc_ns + stream_offset_ns;
}

bool VideoClock::wait_until(int64_t deadline_qpc_ns) noexcept {
    if (stop_.load(std::memory_order_acquire)) return false;

    const int64_t now_qpc = pal::Clock::now_ns();
    const int64_t wait_ns = deadline_qpc_ns - now_qpc;
    // 已过期 — 立即返回(但仍记 drift = 0)。
    if (wait_ns <= 0) {
        stat_last_drift_ns_.store(-wait_ns, std::memory_order_relaxed);
        h_drift().record(std::abs(wait_ns));
        return true;
    }

    std::unique_lock<std::mutex> lk{mu_};
    // 直接 wait 整个 wait_ns,不 cap — cap 会让 T5 处理速率慢于 codec push 速率,
    // 导致 backlog 持续累积直到 drop_oldest。stop/wake 通过 cv.notify_all 即时中断。
    // cv.wait_for 用 steady_clock 内部 timer,精度 ~1ms 在 Windows MSVC(SRWLock+CV)。
    const bool interrupted = cv_.wait_for(
        lk, std::chrono::nanoseconds(wait_ns),
        [this] { return stop_.load(std::memory_order_acquire); });

    const int64_t actual_now = pal::Clock::now_ns();
    const int64_t drift_ns   = actual_now - deadline_qpc_ns;
    stat_last_drift_ns_.store(drift_ns, std::memory_order_relaxed);
    h_drift().record(std::abs(drift_ns));

    if (interrupted) {
        stat_wait_interrupted_.fetch_add(1, std::memory_order_relaxed);
        m_wait_interrupted().inc();
        return false;
    }
    // wait_for 超时返回(谓词 false)→ 到 deadline 了。如果实际 capped 提前返回,
    // 上层下一轮 loop 重新 convert_to_system 即可(deadline 不变,wait 再续)。
    return true;
}

void VideoClock::wake_all() noexcept {
    cv_.notify_all();
}

void VideoClock::request_stop() noexcept {
    stop_.store(true, std::memory_order_release);
    cv_.notify_all();
}

void VideoClock::reset() noexcept {
    std::scoped_lock lk{mu_};
    ctx_ = ClockContext{};            // start_pts_us=INT64_MIN, coeff=1.0, ...
    coeff_avg_.reset_and_fill(1.0);
    stat_coeff_instant_.store(1.0, std::memory_order_relaxed);
    stat_last_drift_ns_.store(0, std::memory_order_relaxed);
    // 不清 stop_(reset 与 stop 解耦);累计 counter 也保留供观测。
    cv_.notify_all();                 // 旧 anchor 的 deadline 失效,wake 等待方
    MCP_LOGF(pal::LogLevel::info, "VideoClock: anchor + coeff reset");
}

VideoClock::Stats VideoClock::stats() const noexcept {
    Stats s{};
    {
        std::scoped_lock lk{mu_};
        s.coeff       = ctx_.coeff;
        s.offset_ns   = ctx_.offset_ns;
        s.last_pts_us = ctx_.last_pts_us;
        s.context_id  = ctx_.context_id;
    }
    s.coeff_instant          = stat_coeff_instant_.load(std::memory_order_relaxed);
    s.last_drift_ns          = stat_last_drift_ns_.load(std::memory_order_relaxed);
    s.coeff_reset_count      = stat_coeff_reset_.load(std::memory_order_relaxed);
    s.discontinuity_count    = stat_discontinuity_.load(std::memory_order_relaxed);
    s.present_count          = stat_present_.load(std::memory_order_relaxed);
    s.wait_interrupted_count = stat_wait_interrupted_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace mcp::media
