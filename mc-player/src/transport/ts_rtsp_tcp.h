/*
 * ts_rtsp_tcp — RTSP/RTP interleaved TCP（RFC 2326 §10.12）。
 *
 * 信令与数据共享单条 TCP 连接，以 `$ <ch> <length(2B)>` 为帧头。
 * 仅 UDP 失败 / SETUP 461 时由 controller 显式选用。
 */

#ifndef MC_PLAYER_TRANSPORT_TS_RTSP_TCP_H_
#define MC_PLAYER_TRANSPORT_TS_RTSP_TCP_H_

#include "transport/transport_session.h"

namespace mcp::transport {

std::unique_ptr<TransportSession> make_ts_rtsp_tcp();

}  // namespace mcp::transport

#endif  // MC_PLAYER_TRANSPORT_TS_RTSP_TCP_H_
