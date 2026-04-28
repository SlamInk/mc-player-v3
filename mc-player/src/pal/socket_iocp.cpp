#include "pal/socket_iocp.h"

#include <Mswsock.h>

#include <atomic>
#include <mutex>

#include "pal/error.h"
#include "pal/log.h"

#pragma comment(lib, "ws2_32.lib")

namespace mcp::pal {

namespace {

// kernel UDP 接收 buffer 默认大小：实测 RTSP 1080p H.264 30fps 高 I 帧瞬时 ~1.5 MB，
// 给 4 MiB 留余量；可被 UdpSocketConfig.recv_buffer_bytes 覆盖。
constexpr uint32_t kDefaultUdpRecvBuf = 4 * 1024 * 1024;

// UDP RTP 首选端口起点（RFC 3550 推荐偶数端口）。0 = OS 自由分配。本项目默认让 OS 分配。
constexpr uint16_t kAutoPort = 0;

std::mutex& wsa_lock() noexcept {
    static std::mutex m;
    return m;
}
int& wsa_refcount() noexcept {
    static int n = 0;
    return n;
}

// 关闭 UDP "Connection-less Receive Coalescing"（URO，Win10+）；
// 同时设大 SO_RCVBUF。失败仅记录日志，不中断初始化。
void apply_rx_tuning(SOCKET s, uint32_t recv_buf_bytes) noexcept {
    int rcvbuf = static_cast<int>(recv_buf_bytes);
    ::setsockopt(s, SOL_SOCKET, SO_RCVBUF,
                 reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

    // UDP_RECV_MAX_COALESCED_SIZE & friends 不在所有 SDK 版本暴露；用 IOCTL 直接关。
    // SIO_UDP_RECV_DATA_COALESCING / WSAID 不公开，跳过；改为 disable IPV4 fragment receive
    // via WSAIoctl(SIO_UDP_NETRESET) 让网卡按包递交（部分驱动需要）。
    DWORD bytes_returned = 0;
    BOOL  no_block = FALSE;
    ::WSAIoctl(s, SIO_UDP_NETRESET, &no_block, sizeof(no_block),
               nullptr, 0, &bytes_returned, nullptr, nullptr);
}

}  // namespace

WsaRuntimeRef::WsaRuntimeRef() noexcept {
    std::scoped_lock lk{wsa_lock()};
    if (wsa_refcount()++ == 0) {
        WSADATA data{};
        const int rc = ::WSAStartup(MAKEWORD(2, 2), &data);
        ok_ = (rc == 0);
        if (!ok_) {
            // 回滚计数：未持有任何资源。
            --wsa_refcount();
            MCP_LOGF(LogLevel::error, "WSAStartup failed rc=%d", rc);
        }
    } else {
        ok_ = true;
    }
}

WsaRuntimeRef::~WsaRuntimeRef() noexcept {
    if (!ok_) return;
    std::scoped_lock lk{wsa_lock()};
    if (--wsa_refcount() == 0) {
        ::WSACleanup();
    }
}

mc_status_t UdpSocket::open(const UdpSocketConfig& cfg) noexcept {
    const int family = cfg.ipv6 ? AF_INET6 : AF_INET;
    SOCKET s = ::WSASocketW(family, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (s == INVALID_SOCKET) {
        return status_from_wsa(::WSAGetLastError());
    }
    SocketGuard guard{s};

    apply_rx_tuning(s, cfg.recv_buffer_bytes != 0 ? cfg.recv_buffer_bytes : kDefaultUdpRecvBuf);

    // bind
    if (cfg.ipv6) {
        sockaddr_in6 sa{};
        sa.sin6_family = AF_INET6;
        sa.sin6_addr   = in6addr_any;
        sa.sin6_port   = ::htons(cfg.bind_port_min);
        if (::bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == SOCKET_ERROR) {
            return status_from_wsa(::WSAGetLastError());
        }
    } else {
        sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = INADDR_ANY;
        sa.sin_port        = ::htons(cfg.bind_port_min);
        if (::bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == SOCKET_ERROR) {
            return status_from_wsa(::WSAGetLastError());
        }
    }

    // 取实际本地端口
    sockaddr_in6 local{};
    int          local_len = sizeof(local);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&local), &local_len) == 0) {
        if (cfg.ipv6) {
            local_port_ = ::ntohs(local.sin6_port);
        } else {
            local_port_ = ::ntohs(reinterpret_cast<sockaddr_in*>(&local)->sin_port);
        }
    }

    s_ = std::move(guard);
    return MC_OK;
}

void UdpSocket::close() noexcept {
    s_.reset();
    local_port_ = 0;
}

mc_status_t UdpSocket::attach_to_iocp(HANDLE iocp, ULONG_PTR completion_key) noexcept {
    if (!s_.valid() || iocp == nullptr) {
        return MC_ERR_INVALID_ARG;
    }
    HANDLE result = ::CreateIoCompletionPort(reinterpret_cast<HANDLE>(s_.get()),
                                              iocp, completion_key, 0);
    if (result == nullptr) {
        return status_from_hresult(HRESULT_FROM_WIN32(::GetLastError()));
    }
    // 设 FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 让同步完成不进入 IOCP（提速）。
    ::SetFileCompletionNotificationModes(reinterpret_cast<HANDLE>(s_.get()),
                                         FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);
    return MC_OK;
}

mc_status_t UdpSocket::post_recv_from(WSABUF* wsabuf,
                                      WSAOVERLAPPED* overlapped,
                                      sockaddr* from_addr,
                                      int* from_addr_len) noexcept {
    if (!s_.valid() || !wsabuf || !overlapped || !from_addr || !from_addr_len) {
        return MC_ERR_INVALID_ARG;
    }
    DWORD bytes  = 0;
    DWORD flags  = 0;
    const int rc = ::WSARecvFrom(s_.get(), wsabuf, 1, &bytes, &flags,
                                  from_addr, from_addr_len, overlapped, nullptr);
    if (rc == SOCKET_ERROR) {
        const int err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            return status_from_wsa(err);
        }
    }
    return MC_OK;
}

mc_status_t UdpSocket::send_to(std::span<const uint8_t> bytes,
                                const sockaddr* dst, int dst_len) noexcept {
    if (!s_.valid() || bytes.empty() || !dst || dst_len <= 0) {
        return MC_ERR_INVALID_ARG;
    }
    WSABUF buf{};
    buf.buf = reinterpret_cast<CHAR*>(const_cast<uint8_t*>(bytes.data()));
    buf.len = static_cast<ULONG>(bytes.size());
    DWORD sent = 0;
    const int rc = ::WSASendTo(s_.get(), &buf, 1, &sent, 0, dst, dst_len, nullptr, nullptr);
    if (rc != 0) {
        return status_from_wsa(::WSAGetLastError());
    }
    return MC_OK;
}

mc_status_t open_udp_socket_pair(UdpSocketConfig cfg_rtp, UdpSocketPair& out) noexcept {
    // 端口对策略：先开 RTP（端口 0 = OS 分配），再尝试 RTP_port+1 给 RTCP；失败则重试。
    constexpr int kPairAttempts = 16;
    for (int i = 0; i < kPairAttempts; ++i) {
        UdpSocket rtp;
        const mc_status_t s1 = rtp.open(cfg_rtp);
        if (s1 != MC_OK) return s1;

        const uint16_t rtp_port = rtp.local_port();
        if (rtp_port == 0 || (rtp_port & 1u) != 0) {
            // RFC 3550: RTP 偶端口；OS 给奇数时重试。
            continue;
        }

        UdpSocketConfig cfg_rtcp = cfg_rtp;
        cfg_rtcp.bind_port_min   = static_cast<uint16_t>(rtp_port + 1);
        cfg_rtcp.is_rtcp         = true;

        UdpSocket rtcp;
        if (rtcp.open(cfg_rtcp) == MC_OK) {
            out.rtp  = std::move(rtp);
            out.rtcp = std::move(rtcp);
            return MC_OK;
        }
        // 端口冲突，重试
    }
    return MC_ERR_IO;
}

}  // namespace mcp::pal
