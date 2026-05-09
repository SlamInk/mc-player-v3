/*
 * Video clock — 完整对齐 VLC `vlc_clock` (`src/clock/clock.c` + `clock_internal.c`)。
 *
 * Master 时序源选择(关键差异):
 *   - VLC 默认 master=audio playback,因为 audio 严格 wall-clock 时序 → coeff EMA 收敛;
 *   - mc-player 无 audio path,改用 RTP arrival_qpc 作为 master(source 推流稳定 30fps,
 *     RTP 第一包到达时刻 ≈ source 推流时刻,wall_diff/stream_diff 严格 ≈ 1.0 收敛);
 *   - codec emit 时刻 / Present 时刻不能当 master:reorder 缓冲 + decode 后是 burst
 *     pattern,instant_coeff 在 0.9~1.4 跳,EMA 永远不收敛(实测 reset_rate 50%)。
 *
 * Deadline 语义:
 *   - convert_to_system(pts) 返回 source 推流到达 wall clock 的时刻(等价 RTP arrival);
 *   - render T5 实际 deadline_present = convert_to_system(pts) + kRenderTargetLatencyNs;
 *   - kRenderTargetLatencyNs ≈ 100ms 是 client buffer(对齐 VLC input_dejitter 默认值),
 *     给 decode + reorder + render 留余量。低于此值大部分帧 wait_ns < 0 立即 present
 *     退化为非 PTS-paced;高于此值客户端延迟过大。
 *
 * 核心算法(VLC `master_update_coeff` 三步走):
 *   1. instant_coeff = system_diff / stream_diff * rate(本帧实测频率比);
 *   2. EMA 平滑(`Average` range=10),COEFF_THRESHOLD=0.2 异常重置;
 *   3. 重算 offset:offset = system_now - (ts - start_time.stream)*coeff/rate - start_time.system;
 *
 * Convert(VLC `context_stream_to_system`):
 *   system = (ts - start_time.stream)*coeff/rate + offset + start_time.system
 *
 * Wait(VLC `vlc_clock_Wait`):cv.wait_for(deadline - now) 可被 wake_all/stop 中断。
 *
 * Context 切换(VLC `prev_contexts` 链表):
 *   PTS 跳变 |Δ|>200ms → context_id++ 重置 coeff=1.0 / coeff_avg fill_with(1.0) / start_time
 *   重锚定到当前 (now, pts)。简化版不保留历史 context(没 seek-back 场景)。
 *
 * 不变量:
 *   - 单一互斥 mu_,master_update / convert_to_system / wait_until / reset 全互斥;
 *   - Bootstrap 前(start_pts_us == INT64_MIN)convert_to_system 退化为 monotonic now_qpc;
 *   - 所有时间:pts_us 单位 us / qpc_ns 单位 ns / coeff/rate 无量纲 double;
 *   - QPC 与 std::condition_variable 时基:cv.wait_for 用 steady_clock,wait_for(wait_ns) =
 *     等待 wait_ns 真实时间 ≈ QPC 同等时长,误差 1-2ms 量级足够 PTS-paced 用。
 *
 * 不实装(Phase 3+ 升级路径):
 *   - audio master + slave on_update 反馈机制(我们现在 video 自己当 master);
 *   - input_master(RTCP-SR ntp/rtp_ts 拟合)— 等我们在 controller 接 RTCP-SR 后升级;
 *   - prev_contexts 历史链 — 没 seek-back 需求;
 *   - SPU slave + delay sync — 字幕未实装。
 */

#ifndef MC_PLAYER_MEDIA_VIDEO_CLOCK_H_
#define MC_PLAYER_MEDIA_VIDEO_CLOCK_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace mcp::media {

/// 滑窗 EMA — 对齐 VLC `clock_internal.c` AvgUpdate / AvgGet。range=10 时
/// 等价"最近 10 帧 instant_coeff 均值",收敛速度 ~10 帧。
class Average {
public:
    static constexpr int kRange = 10;

    /// 累积一个新值。前 kRange 次按 incremental average 计;满 kRange 后变 sliding。
    /// 公式(VLC AvgUpdate):
    ///   weight = (count < range ? count++ : range - 1)
    ///   divisor = (count < range ? count : range)
    ///   value = (weight * value + new_value) / divisor
    void update(double value) noexcept;

    /// 重置并预填(对齐 VLC AvgResetAndFill):value = v, count = range(立即满)。
    void reset_and_fill(double v) noexcept;

    [[nodiscard]] double get() const noexcept { return value_; }
    [[nodiscard]] int    count() const noexcept { return count_; }

private:
    double value_ = 1.0;
    int    count_ = 0;
};

/// 时钟 context — 对齐 VLC `vlc_clock_context`。每次 PTS 不连续创建新 context_id。
struct ClockContext {
    double  coeff        = 1.0;          // EMA 平滑后的 stream→system 频率比
    double  rate         = 1.0;          // 播放速率(1.0 正常,0.5 慢放)
    int64_t offset_ns    = 0;            // baseline offset(ComputeOffset 重算)
    int64_t start_pts_us = INT64_MIN;    // bootstrap 锚定 stream
    int64_t start_qpc_ns = 0;            // bootstrap 锚定 system
    int64_t last_pts_us  = INT64_MIN;    // 上次 master_update 的 stream
    int64_t last_qpc_ns  = 0;            // 上次 master_update 的 system
    uint32_t context_id  = 0;            // PTS 不连续 → ++
};

class VideoClock {
public:
    VideoClock() noexcept;
    ~VideoClock() noexcept;

    VideoClock(const VideoClock&)            = delete;
    VideoClock& operator=(const VideoClock&) = delete;

    /// Master 路径 — 用 RTP arrival_qpc(source 推流到达 wall clock)+ pts 学 EMA。
    /// 对齐 VLC `master_update_coeff` 三步:instant_coeff → EMA/threshold → ComputeOffset。
    /// rate=1.0 默认正常播放;变速时调用方传入。
    /// 注意:不要传 codec emit 时刻或 Present 时刻 — 那是 burst 数据,EMA 不收敛。
    void master_update(int64_t arrival_qpc_ns, int64_t pts_us, double rate = 1.0) noexcept;

    /// Slave 路径(无 audio 时由本类自己当 master,直接复用 ctx 的 coeff/offset)。
    /// 对齐 VLC `context_stream_to_system`:
    ///   system = (pts - start_time.stream)*coeff/rate + offset + start_time.system
    /// Bootstrap 前返回 now_qpc_fallback(立即 present)。
    [[nodiscard]] int64_t convert_to_system(int64_t pts_us,
                                             int64_t now_qpc_fallback,
                                             double rate = 1.0) const noexcept;

    /// 阻塞到 deadline_qpc_ns 或被 wake_all/stop 打断。对齐 VLC `vlc_clock_Wait`。
    /// 返回 true = 到时间(或已过期);false = 被打断(stop/wake)。
    /// deadline ≤ now 立即返回 true(已过期)。
    bool wait_until(int64_t deadline_qpc_ns) noexcept;

    /// 唤醒所有 wait_until 的 caller(rate 变 / context 切 / stop 时调)。
    /// 对齐 VLC `vlc_clock_Wake`(cv_broadcast)。
    void wake_all() noexcept;

    /// stop 信号 — 让所有 wait_until return false,后续 wait_until 直接 return false。
    /// device-lost / session 析构时调。
    void request_stop() noexcept;

    /// 清 anchor + coeff,下一次 master_update 重新 bootstrap。reset 不影响 stop 状态。
    void reset() noexcept;

    /// 当前 anchor 已建立?
    [[nodiscard]] bool has_anchor() const noexcept;

    struct Stats {
        double   coeff;
        double   coeff_instant;          // 最近一次 instant_coeff(未平滑)
        int64_t  offset_ns;
        int64_t  last_pts_us;
        int64_t  last_drift_ns;          // 最近一次 wait 后 actual - deadline
        uint32_t context_id;
        uint64_t coeff_reset_count;      // |instant_coeff-1| > THRESHOLD 触发 reset 次数
        uint64_t discontinuity_count;    // PTS 跳变触发 context 切换次数
        uint64_t present_count;
        uint64_t wait_interrupted_count; // wait_until 被 wake/stop 打断次数
    };
    [[nodiscard]] Stats stats() const noexcept;

    static constexpr double  kCoeffThreshold  = 0.2;       // VLC COEFF_THRESHOLD
    static constexpr int64_t kDiscontinuityUs = 200'000;   // 30fps 33-67ms,3x 余量
    /// Client buffer:deadline_present = master.convert_to_system(pts) + 此值。
    /// 实测约束(LAN rtsp 30fps):
    ///   - decode_delay (RTP arrival → frame popped from render queue) ≈ 35-45ms;
    ///   - T5 处理时间 = wait + present,present ≈ 5ms;
    ///   - 必须 T5 < codec push interval (33ms) 否则 backlog 累积:
    ///     wait + 5 < 33 → wait < 28 → render_target < decode + 28 = 60-70ms。
    /// 取 60ms:wait 平均 ~20ms,T5 ~25ms < codec 33ms,backlog 自然 drain。
    /// 客户端总延迟 ≈ jitter 30 + decode 40 + render_target 60 = 130ms 仍可接受。
    static constexpr int64_t kRenderTargetLatencyNs = 60'000'000LL;     // 60ms

private:
    mutable std::mutex      mu_;
    std::condition_variable cv_;
    ClockContext            ctx_;
    Average                 coeff_avg_;
    std::atomic<bool>       stop_{false};

    // 观测/统计(写都在 mu_ 内;读用 atomic 给 stats() 跨线程)
    std::atomic<double>   stat_coeff_instant_{1.0};
    std::atomic<int64_t>  stat_last_drift_ns_{0};
    std::atomic<uint64_t> stat_coeff_reset_{0};
    std::atomic<uint64_t> stat_discontinuity_{0};
    std::atomic<uint64_t> stat_present_{0};
    std::atomic<uint64_t> stat_wait_interrupted_{0};
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_VIDEO_CLOCK_H_
