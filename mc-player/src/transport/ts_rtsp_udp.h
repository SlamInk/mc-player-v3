/*
 * ts_rtsp_udp — RTSP TCP 信令 + UDP RTP/RTCP 数据。
 *
 * 拓扑：
 *   T1 (Signaling)        : 同步 TCP socket，RtspClient 调度 OPTIONS/DESCRIBE/SETUP/PLAY/keepalive。
 *   T2 (Network RX)       : IOCP 单线程，聚合 video RTP / video RTCP / audio RTP / audio RTCP 4 个 UDP socket 的完成事件。
 *   keepalive 计时        : T1 同线程上自管 sleep，无独立线程。
 *
 * 失败降级：UDP socket 创建失败、SETUP 返回 461 → 上抛 MC_ERR_UNSUPPORTED，由工厂层切 ts_rtsp_tcp。
 */

#ifndef MC_PLAYER_TRANSPORT_TS_RTSP_UDP_H_
#define MC_PLAYER_TRANSPORT_TS_RTSP_UDP_H_

#include "transport/transport_session.h"

namespace mcp::transport {

std::unique_ptr<TransportSession> make_ts_rtsp_udp();

}  // namespace mcp::transport

#endif  // MC_PLAYER_TRANSPORT_TS_RTSP_UDP_H_
