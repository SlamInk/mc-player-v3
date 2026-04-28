/*
 * Transport Session 抽象 — RTSP UDP / RTSP TCP / WHEP 的协议无关接口。
 *
 * ADD §3.4：协议在 jitter buffer 之上完全合流。Transport Session 把信令 + 加密 + socket 封装，
 * 对上输出统一的「RTP datagram + RTCP 事件 + 流元数据」。
 *
 * v1 仅启用 ts_rtsp_udp / ts_rtsp_tcp；ts_whep 留接口，不实现。
 */

#ifndef MC_PLAYER_TRANSPORT_TRANSPORT_SESSION_H_
#define MC_PLAYER_TRANSPORT_TRANSPORT_SESSION_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "mc-player/mc_player_types.h"
#include "transport/rtcp.h"
#include "transport/sdp_parser.h"

namespace mcp::transport {

enum class MediaKind { video, audio };

struct RtpDatagram {
    MediaKind                   kind;
    int64_t                     arrival_qpc_ns = 0;
    std::vector<uint8_t>        bytes;          // 单 RTP datagram；ts_rtsp_tcp 已剔除 4 字节 interleaved 头
};

struct StreamDescription {
    SdpSession                  sdp;
    std::string                 base_uri;       // 用于解析相对 a=control 的 base
    bool                        rtcp_mux        = false;     // RTSP 默认双 socket，false；WHEP true
};

/// Transport Session 事件回调集合 — caller 单独提供。
struct TransportSessionSink {
    /// SDP / 流元数据可用（DESCRIBE 200 之后）。一次会话只触发一次。
    std::function<void(const StreamDescription&)> on_stream_ready;

    /// 一个 RTP datagram 到达。本回调在 T2 (Network RX) 线程触发，无锁；caller 必须低延时 enqueue。
    std::function<void(const RtpDatagram&)>       on_rtp;

    /// 入站 RTCP 解码事件（按出现次数堆叠到对应 vector）。
    std::function<void(const std::vector<RtcpSenderReport>&,
                       const std::vector<RtcpReceiverReport>&,
                       const std::vector<RtcpFeedbackNack>&,
                       const std::vector<RtcpFeedbackPli>&)> on_rtcp;

    /// 致命错误。Transport Session 进入终结态前最后一次 callback。
    std::function<void(mc_status_t, std::string)> on_error;

    /// 网络层异常（短时不可用，不一定 fatal）。
    std::function<void()>                         on_network_down;
    std::function<void()>                         on_network_up;
};

class TransportSession {
public:
    virtual ~TransportSession() noexcept = default;

    virtual mc_status_t open(const std::string& url,
                              const std::string& username,
                              const std::string& password,
                              uint32_t connect_timeout_ms,
                              uint32_t read_timeout_ms,
                              uint32_t keepalive_interval_ms) noexcept = 0;

    virtual void close() noexcept = 0;

    virtual void set_sink(TransportSessionSink sink) noexcept = 0;

    /// 出站 RTCP 复合包；内部按 kind 路由到 video / audio 的 RTCP 通道。
    virtual mc_status_t send_rtcp(MediaKind kind, std::span<const uint8_t> bytes) noexcept = 0;
};

/// 工厂 — 按 protocol_hint 选择具体实现。AUTO 时按 URL scheme + UDP 失败降级 TCP。
std::unique_ptr<TransportSession> make_rtsp_transport(mc_protocol_t hint, bool allow_tcp_fallback);

}  // namespace mcp::transport

#endif  // MC_PLAYER_TRANSPORT_TRANSPORT_SESSION_H_
