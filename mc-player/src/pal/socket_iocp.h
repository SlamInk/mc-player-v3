/*
 * IOCP UDP socket 原语 — RTSP/RTP UDP 收包专用。
 *
 * 设计：
 *   - 每个 session 创建一对 UDP socket（RTP / RTCP）。
 *   - 单 IOCP 端口聚合 RTP+RTCP+任何 RTSP TCP 通道，供 T2 (Network RX) 唯一线程拉取。
 *   - 接收用 WSARecvFrom + IOCP 完成包通知（per-packet completion）。
 *   - 发送用同步 WSASendTo（RTCP 频率低，不值得 IOCP）。
 *   - kernel SO_RCVBUF 调到大值（4 MiB）；UDP 关闭 URO（Connection-less Receive Coalescing）。
 *
 * ADD §4.1 规定：大 SO_RCVBUF；URO 关闭；Interrupt Moderation 关（IM 在网卡级，本模块管不了）。
 */

#ifndef MC_PLAYER_PAL_SOCKET_IOCP_H_
#define MC_PLAYER_PAL_SOCKET_IOCP_H_

#include <Windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <span>
#include <vector>

#include "mc-player/mc_player_types.h"
#include "pal/raii.h"

namespace mcp::pal {

/// Winsock global 引用计数（WSAStartup / WSACleanup）。
class WsaRuntimeRef {
public:
    WsaRuntimeRef() noexcept;
    ~WsaRuntimeRef() noexcept;
    WsaRuntimeRef(const WsaRuntimeRef&)            = delete;
    WsaRuntimeRef& operator=(const WsaRuntimeRef&) = delete;
    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    bool ok_ = false;
};

struct UdpSocketConfig {
    uint32_t recv_buffer_bytes  = 0;    // 0 = 走库默认（命名常量 kDefaultUdpRecvBuf）
    uint16_t bind_port_min      = 0;    // 0 = 让 OS 选择
    uint16_t bind_port_max      = 0;    // 与 min 一起表示偶数端口范围（RTP=偶，RTCP=偶+1）
    bool     bind_pair          = false;// 同时拿一对偶数+奇数端口（RTSP RTP/RTCP 习惯）
    bool     is_rtcp            = false;
    bool     ipv6               = false;
};

/// 单个 UDP socket（IOCP 关联在 SocketGroup 层做）。
class UdpSocket {
public:
    UdpSocket() noexcept = default;
    UdpSocket(const UdpSocket&)            = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&&) noexcept        = default;
    UdpSocket& operator=(UdpSocket&&) noexcept = default;

    mc_status_t open(const UdpSocketConfig& cfg) noexcept;
    void close() noexcept;

    [[nodiscard]] SOCKET native() const noexcept { return s_.get(); }
    [[nodiscard]] uint16_t local_port() const noexcept { return local_port_; }
    [[nodiscard]] bool valid() const noexcept { return s_.valid(); }

    /// 把 socket attach 到 IOCP；key 为 caller 自由使用的标识。
    mc_status_t attach_to_iocp(HANDLE iocp, ULONG_PTR completion_key) noexcept;

    /// 用 WSARecvFrom 投递一次接收（buf 必须由 caller 持有直到完成）。
    /// 完成时通过 GetQueuedCompletionStatus 取回。
    /// `overlapped` 与 `from_addr` / `from_addr_len` 必须与 buf 同生命周期。
    mc_status_t post_recv_from(WSABUF* wsabuf,
                               WSAOVERLAPPED* overlapped,
                               sockaddr* from_addr,
                               int* from_addr_len) noexcept;

    /// 同步发送（RTCP 用）。
    mc_status_t send_to(std::span<const uint8_t> bytes,
                        const sockaddr* dst, int dst_len) noexcept;

private:
    SocketGuard s_;
    uint16_t    local_port_ = 0;
};

/// 按 RTSP 习惯申请一对 RTP/RTCP socket（端口偶数+1）。
struct UdpSocketPair {
    UdpSocket rtp;
    UdpSocket rtcp;
};

mc_status_t open_udp_socket_pair(UdpSocketConfig cfg_rtp, UdpSocketPair& out) noexcept;

}  // namespace mcp::pal

#endif  // MC_PLAYER_PAL_SOCKET_IOCP_H_
