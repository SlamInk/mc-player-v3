/*
 * Network Probe — RTT / jitter / loss / link_kind 推断(ADR-018 / capability_probe §3.3)。
 *
 * 数据源:
 *   - RTSP keepalive RTT 或 RTCP RR/SR 反推(transport 层钩子)
 *   - 首 GOP 内 RTP iat 抖动 + loss 统计(jitter_buffer 钩子)
 *
 * 实装边界(Phase 9.0 结构性骨架):
 *   - 接口骨架 + record_*() 钩子
 *   - link_kind 推断算法(ADD §7.5.2 阈值表)留 9.0 主线后续
 */

#ifndef MC_PLAYER_PROBE_NETWORK_PROBE_H_
#define MC_PLAYER_PROBE_NETWORK_PROBE_H_

#include <cstdint>
#include <memory>

#include "probe/capability_snapshot.h"

namespace mcp::probe {

class NetworkProbe {
public:
    NetworkProbe();
    ~NetworkProbe();

    NetworkProbe(const NetworkProbe&)            = delete;
    NetworkProbe& operator=(const NetworkProbe&) = delete;

    /// transport 层每次收 RTCP RR/SR 调一次。
    void record_rtt_sample_ns(int64_t rtt_ns) noexcept;

    /// jitter_buffer 每收一个 RTP 包调一次。
    void record_packet(int64_t arrival_ns, uint16_t seq, uint32_t rtp_ts) noexcept;

    /// 首 GOP 完成时(jitter_buffer 调一次)产出 NetworkSnapshot。
    [[nodiscard]] NetworkSnapshot snapshot() const noexcept;

    /// 是否已完成首 GOP 数据收集。
    [[nodiscard]] bool ready() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp::probe

#endif  // MC_PLAYER_PROBE_NETWORK_PROBE_H_
