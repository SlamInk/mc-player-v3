/*
 * 视频 RTP jitter buffer — 短延时 reorder + RTCP NACK 触发(对齐 VLC live555 行为)。
 *
 * 设计目标:
 *   - 把 RTP datagram 按 sequence number 升序、连续地输出给上层 depack;
 *   - 乱序到达的包(network reorder)在 dwell window 内重排;
 *   - 真丢失的包通过 RTCP Generic NACK(RFC 4585 §6.2.1)请求重传;
 *   - 超过 dwell window 仍未到达的 seq 标记为永久丢失,跳过该 seq 让管道继续;
 *   - 重复包(NACK 重传到达,seq 已 emit)静默丢弃。
 *
 * 不变量:
 *   - emit 顺序严格按 seq 升序、连续(中间空位会等到 dwell 超时才跳过);
 *   - 32-bit extended seq 处理 RTP 16-bit wrap(每 13.6 小时 @ 90kHz 一次);
 *   - 单线程 on_rtp(由 T2 RX 调用);stats 暴露原子读取给控制台 1Hz tick。
 *
 * 不实装(留给后续阶段):
 *   - Kalman frame_delay_variation 估计(ADD §5.3.1);当前 fixed dwell。
 *   - target_delay 自适应;当前固定 30 ms(IPC LAN 实测覆盖 95%+ jitter)。
 */

#ifndef MC_PLAYER_MEDIA_JITTER_BUFFER_VIDEO_H_
#define MC_PLAYER_MEDIA_JITTER_BUFFER_VIDEO_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include "transport/transport_session.h"

namespace mcp::media {

class JitterBufferVideo {
public:
    /// emit datagram 给 depack(按 seq 升序、可能跳过永久丢失的 seq)。
    using EmitFn = std::function<void(const transport::RtpDatagram&)>;

    /// 触发 RTCP Generic NACK(RFC 4585 §6.2.1):pid + blp 16-bit bitmap。
    /// pid = 首个 lost seq;blp 表示后续 16 个 seq 哪些也 lost(bit i = pid+1+i)。
    using NackFn = std::function<void(uint16_t pid, uint16_t blp)>;

    /// 真丢失上报 — dwell 超时后该 seq 仍没到,通知 controller 让 gate poison。
    /// expired_seq 是最旧的被跳过的 seq;如果连续多个跳过,逐个上报。
    using LossFn = std::function<void(uint16_t expired_seq, uint32_t count)>;

    JitterBufferVideo(EmitFn emit, NackFn nack, LossFn loss) noexcept;
    ~JitterBufferVideo() noexcept;

    JitterBufferVideo(const JitterBufferVideo&)            = delete;
    JitterBufferVideo& operator=(const JitterBufferVideo&) = delete;

    /// dwell:单包最大滞留时间(超此时间未到的 seq 跳过 emit),典型 30ms。
    /// nack_min_interval:同一 pid 重发 NACK 的最小间隔(限频),典型 50ms。
    /// 0 = 用内置默认。
    void configure(int64_t dwell_us, int64_t nack_min_interval_us) noexcept;

    /// 输入 RTP datagram(由 T2 RX 单线程调用)。
    void on_rtp(const transport::RtpDatagram& dg) noexcept;

    /// 周期 tick(典型 1Hz / 60Hz),触发 dwell 超时 sweep。on_rtp 也会内联 check
    /// 但稀疏流(暂停后恢复)需要 tick 推进。
    void tick(int64_t now_qpc_ns) noexcept;

    /// 重连/device-lost 时清空(下一个 RTP 重新 bootstrap)。
    void reset() noexcept;

    struct Stats {
        uint64_t emits;
        uint64_t late_drops;        // 迟到包(seq < next_expected),drop
        uint64_t duplicate_drops;   // 重复包(NACK 重传到达,slot 已 emit/已填),drop
        uint64_t expired_gaps;      // dwell 超时跳过的 seq 数
        uint64_t nacks_sent;
        uint64_t reorder_events;    // 真乱序到达(seq 入队时 < highest_seen_)的次数
        uint32_t pending_count;     // 当前队列里待 emit 的包数
        uint32_t max_pending_seen;  // 历史最大 pending(诊断 buffer 是否太小)
    };
    [[nodiscard]] Stats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_JITTER_BUFFER_VIDEO_H_
