/*
 * RTCP — SR / RR / SDES / RTPFB-NACK / PSFB-PLI 解析与构造（RFC 3550 + RFC 4585 + RFC 5104）。
 *
 * 复合包：以 SR 或 RR 起头，后接 SDES（必须），再追加任意数量 RTPFB / PSFB。
 *
 * AVPF 反馈策略采用 Immediate Feedback mode + SDP `a=rtcp-fb:* trr-int 0`（ADD §5.4）；
 * 这两个机制相互独立，需同时满足。
 */

#ifndef MC_PLAYER_TRANSPORT_RTCP_H_
#define MC_PLAYER_TRANSPORT_RTCP_H_

#include <cstdint>
#include <span>
#include <vector>

namespace mcp::transport {

struct RtcpSenderReport {
    uint32_t ssrc            = 0;
    uint64_t ntp_timestamp   = 0;     // 64-bit NTP (high 32 = seconds)
    uint32_t rtp_timestamp   = 0;     // 同流 SSRC 的 RTP 时间戳（NTP-RTP 线性回归输入）
    uint32_t packet_count    = 0;
    uint32_t octet_count     = 0;
};

struct RtcpReceiverReportBlock {
    uint32_t ssrc                   = 0;
    uint8_t  fraction_lost          = 0;
    int32_t  cumulative_lost        = 0;    // 24-bit signed
    uint32_t extended_highest_seq   = 0;
    uint32_t interarrival_jitter    = 0;
    uint32_t last_sr                = 0;    // middle 32 bits of NTP from SR
    uint32_t delay_since_last_sr    = 0;    // 1/65536 second units
};

struct RtcpReceiverReport {
    uint32_t ssrc = 0;
    std::vector<RtcpReceiverReportBlock> blocks;
};

struct RtcpNackEntry {
    uint16_t pid  = 0;
    uint16_t blp  = 0;
};

struct RtcpFeedbackNack {
    uint32_t sender_ssrc = 0;
    uint32_t media_ssrc  = 0;
    std::vector<RtcpNackEntry> entries;
};

struct RtcpFeedbackPli {
    uint32_t sender_ssrc = 0;
    uint32_t media_ssrc  = 0;
};

class RtcpReader {
public:
    /// 解析一个复合 RTCP 包。失败返回 false。所有事件按出现顺序追加到对应 vector 上。
    bool parse(std::span<const uint8_t> bytes,
               std::vector<RtcpSenderReport>& srs,
               std::vector<RtcpReceiverReport>& rrs,
               std::vector<RtcpFeedbackNack>& nacks,
               std::vector<RtcpFeedbackPli>& plis) noexcept;
};

class RtcpWriter {
public:
    /// 把 RR 块编入 buffer，返回写入字节数。capacity 不足返回 0。
    static std::size_t write_receiver_report(uint32_t sender_ssrc,
                                              std::span<const RtcpReceiverReportBlock> blocks,
                                              std::span<uint8_t> out) noexcept;

    /// 写 SDES with single CNAME（RFC 3550 §6.5）。
    static std::size_t write_sdes_cname(uint32_t sender_ssrc,
                                         std::span<const char> cname,
                                         std::span<uint8_t> out) noexcept;

    /// 写 RTPFB-NACK（RFC 4585 §6.2.1）。
    static std::size_t write_nack(uint32_t sender_ssrc, uint32_t media_ssrc,
                                   std::span<const RtcpNackEntry> entries,
                                   std::span<uint8_t> out) noexcept;

    /// 写 PSFB-PLI（RFC 4585 §6.3.1）。
    static std::size_t write_pli(uint32_t sender_ssrc, uint32_t media_ssrc,
                                  std::span<uint8_t> out) noexcept;
};

}  // namespace mcp::transport

#endif  // MC_PLAYER_TRANSPORT_RTCP_H_
