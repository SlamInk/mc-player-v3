#include "transport/ts_rtsp_udp.h"

#include <Windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "pal/clock.h"
#include "pal/error.h"
#include "pal/log.h"
#include "pal/raii.h"
#include "pal/socket_iocp.h"
#include "pal/thread.h"
#include "transport/rtsp_client.h"
#include "transport/sdp_parser.h"
#include "transport/ts_rtsp_tcp.h"

namespace mcp::transport {

namespace {

constexpr int kKeepaliveDivisor = 2;            // session timeout 的几分之一发一次
constexpr uint32_t kDefaultKeepaliveMs = 25'000;

constexpr std::size_t kRxBufferBytes = 64 * 1024;     // 单个 UDP datagram 最大 ≈ 1500，4 socket 共 4×64KB

struct ParsedUrl {
    std::string scheme;
    std::string user;
    std::string pass;
    std::string host;
    uint16_t    port{0};
    std::string path;       // 含 leading "/"
};

bool parse_rtsp_url(std::string_view url, ParsedUrl& out) noexcept {
    // 防御 caller 传入含 leading/trailing whitespace 的 URL(命令行残留 / 配置文件粘贴)。
    // RFC 3986 §3 URI 不允许 unencoded whitespace,这里直接 trim 容忍而不是拒绝,
    // 否则 SETUP 拼接 control_uri 时空格会塞进 URI 中段触发 server FIN(实测华为/海康
    // IPC 表现:OPTIONS/DESCRIBE 容忍尾部空格回 200,SETUP 拼出 host 后空格直接关连接)。
    auto is_ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
    };
    while (!url.empty() && is_ws(url.front())) url.remove_prefix(1);
    while (!url.empty() && is_ws(url.back()))  url.remove_suffix(1);

    auto sch_end = url.find("://");
    if (sch_end == std::string_view::npos) return false;
    out.scheme.assign(url.substr(0, sch_end));
    auto rest = url.substr(sch_end + 3);

    auto path_pos = rest.find('/');
    std::string_view authority = (path_pos == std::string_view::npos) ? rest : rest.substr(0, path_pos);
    out.path.assign(path_pos == std::string_view::npos ? std::string_view{"/"} : rest.substr(path_pos));

    auto at = authority.find('@');
    if (at != std::string_view::npos) {
        auto creds = authority.substr(0, at);
        authority  = authority.substr(at + 1);
        auto colon = creds.find(':');
        if (colon != std::string_view::npos) {
            out.user.assign(creds.substr(0, colon));
            out.pass.assign(creds.substr(colon + 1));
        } else {
            out.user.assign(creds);
        }
    }

    auto col = authority.rfind(':');
    if (col != std::string_view::npos) {
        out.host.assign(authority.substr(0, col));
        uint32_t p = 0;
        auto port_sv = authority.substr(col + 1);
        std::from_chars(port_sv.data(), port_sv.data() + port_sv.size(), p);
        out.port = static_cast<uint16_t>(p & 0xFFFF);
    } else {
        out.host.assign(authority);
        out.port = (out.scheme == "rtsps") ? 322 : 554;
    }
    return !out.host.empty();
}

class TcpStream {
public:
    TcpStream() = default;
    ~TcpStream() { close(); }

    mc_status_t connect(const std::string& host, uint16_t port, uint32_t timeout_ms) noexcept {
        addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        char port_buf[16];
        std::snprintf(port_buf, sizeof(port_buf), "%u", static_cast<unsigned>(port));

        addrinfo* res = nullptr;
        if (::getaddrinfo(host.c_str(), port_buf, &hints, &res) != 0 || !res) {
            return MC_ERR_IO;
        }
        pal::ScopeExit free_res{[res] { ::freeaddrinfo(res); }};

        SOCKET s = INVALID_SOCKET;
        for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
            s = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (s == INVALID_SOCKET) continue;

            // 非阻塞 connect + select 等可写以支持超时；简化：直接阻塞 + recv 超时由 SO_RCVTIMEO/SO_SNDTIMEO 控制。
            DWORD tv = timeout_ms;
            ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                          reinterpret_cast<const char*>(&tv), sizeof(tv));
            ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
                          reinterpret_cast<const char*>(&tv), sizeof(tv));

            if (::connect(s, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) {
                break;
            }
            ::closesocket(s);
            s = INVALID_SOCKET;
        }
        if (s == INVALID_SOCKET) return MC_ERR_IO;
        s_.reset(s);

        // TCP_NODELAY：信令延时敏感。
        BOOL nodelay = TRUE;
        ::setsockopt(s_.get(), IPPROTO_TCP, TCP_NODELAY,
                      reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
        return MC_OK;
    }

    void close() noexcept { s_.reset(); }

    bool send_all(std::span<const uint8_t> bytes) noexcept {
        if (!s_.valid()) {
            MCP_LOG_WARN("TcpStream::send_all socket invalid");
            return false;
        }
        const uint8_t* p = bytes.data();
        std::size_t   n = bytes.size();
        while (n > 0) {
            int sent = ::send(s_.get(), reinterpret_cast<const char*>(p),
                               static_cast<int>(std::min<std::size_t>(n, INT_MAX)), 0);
            if (sent <= 0) {
                MCP_LOGF(pal::LogLevel::warn,
                         "TcpStream::send_all send failed sent=%d wsa_err=%d "
                         "(remaining=%zu)",
                         sent, ::WSAGetLastError(), n);
                return false;
            }
            p += sent;
            n -= static_cast<std::size_t>(sent);
        }
        return true;
    }

    bool read_some(std::vector<uint8_t>& out) noexcept {
        if (!s_.valid()) {
            MCP_LOG_WARN("TcpStream::read_some socket invalid");
            return false;
        }
        out.resize(4096);
        int n = ::recv(s_.get(), reinterpret_cast<char*>(out.data()),
                        static_cast<int>(out.size()), 0);
        if (n == 0) {
            MCP_LOG_WARN("TcpStream::read_some recv=0 (FIN, socket closed by peer)");
            return false;
        }
        if (n < 0) {
            MCP_LOGF(pal::LogLevel::warn,
                     "TcpStream::read_some recv failed wsa_err=%d", ::WSAGetLastError());
            return false;
        }
        out.resize(static_cast<std::size_t>(n));
        return true;
    }

    // 取已 connect 成功的 peer 地址 — 上层用其 IP + RTSP SETUP 解出的 server_rtcp_port
    // prime UDP RTCP 通道的 peer_addr,这样首个 PLI 不必等 server 发过来一个 RTCP 学路由。
    bool peer_address(sockaddr_storage& out, int& out_len) const noexcept {
        if (!s_.valid()) return false;
        out_len = sizeof(out);
        return ::getpeername(s_.get(),
                              reinterpret_cast<sockaddr*>(&out),
                              &out_len) == 0;
    }

private:
    pal::SocketGuard s_;
};

class TsRtspUdp final : public TransportSession {
public:
    TsRtspUdp() = default;
    ~TsRtspUdp() override { close(); }

    void set_sink(TransportSessionSink sink) noexcept override {
        std::scoped_lock lk{mu_};
        sink_ = std::move(sink);
    }

    mc_status_t open(const std::string& url,
                      const std::string& username,
                      const std::string& password,
                      uint32_t connect_timeout_ms,
                      uint32_t /*read_timeout_ms*/,
                      uint32_t keepalive_interval_ms) noexcept override {
        if (running_.load(std::memory_order_acquire)) return MC_ERR_INVALID_STATE;

        ParsedUrl u;
        if (!parse_rtsp_url(url, u)) return MC_ERR_INVALID_ARG;

        const std::string user = !username.empty() ? username : u.user;
        const std::string pass = !password.empty() ? password : u.pass;

        if (mc_status_t s = signaling_.connect(u.host, u.port, connect_timeout_ms); s != MC_OK) {
            return s;
        }

        // 重组无 user:pass 的 base URL，用作 RTSP 请求的 URI。
        std::string base_url = u.scheme + "://" + u.host;
        if ((u.scheme == "rtsp"  && u.port != 554) ||
            (u.scheme == "rtsps" && u.port != 322)) {
            base_url += ":" + std::to_string(u.port);
        }
        base_url += u.path;
        base_url_ = base_url;

        client_.set_credentials(user, pass);
        RtspByteIo io;
        io.send       = [this](std::span<const uint8_t> b) { return signaling_.send_all(b); };
        io.read_some  = [this](std::vector<uint8_t>& out) { return signaling_.read_some(out); };
        io.shutdown   = [this] { signaling_.close(); };
        client_.set_byte_io(std::move(io));

        SdpSession sdp;
        if (mc_status_t s = client_.do_describe(base_url_, connect_timeout_ms, sdp); s != MC_OK) {
            close();
            return s;
        }

        // 为 video / audio 分别申请 UDP 端口对，然后 SETUP。
        for (auto& m : sdp.media) {
            if (m.kind != SdpMedia::Kind::video && m.kind != SdpMedia::Kind::audio) continue;

            pal::UdpSocketPair pair;
            pal::UdpSocketConfig cfg_rtp;
            cfg_rtp.recv_buffer_bytes = 0;          // 用 PAL 默认（4 MiB）
            if (mc_status_t s = pal::open_udp_socket_pair(cfg_rtp, pair); s != MC_OK) {
                close();
                return MC_ERR_UNSUPPORTED;          // 让 controller 决定降级 TCP
            }

            RtspSetupTarget tgt;
            tgt.kind             = m.kind;
            tgt.control_uri      = m.control_uri;
            tgt.client_rtp_port  = pair.rtp.local_port();
            tgt.client_rtcp_port = pair.rtcp.local_port();
            // 第二次起 SETUP 必须带首次得到的 Session id（RFC 2326 §12.37）。
            if (!channels_.empty()) {
                tgt.existing_session_id = channels_.front().session_id;
            }

            RtspSetupResult result;
            if (mc_status_t s = client_.do_setup(base_url_, tgt, connect_timeout_ms, result); s != MC_OK) {
                close();
                return s;
            }

            MediaChannel mc;
            mc.kind            = m.kind;
            mc.session_id      = result.session_id;
            mc.timeout_seconds = result.timeout_seconds;
            mc.rtp             = std::move(pair.rtp);
            mc.rtcp            = std::move(pair.rtcp);

            // Bootstrap RTCP peer addr:server IP 取自 signaling TCP getpeername,
            // RTCP port 取自 SETUP response Transport: server_port=A-B 的 B。
            // 不做这步首帧 PLI 会 send 失败(MC_ERR_INVALID_STATE),原 RX 路径只在
            // 收到 server 主动发 RTCP SR 才学到 peer_addr → 摄像头默认 5+s 才下首
            // SR,期间 PLI 全丢 → controller 的 SEQ_GAP poison 永远等不到 IDR。
            if (result.server_rtcp_port != 0) {
                sockaddr_storage signaling_peer{};
                int              signaling_peer_len = 0;
                if (signaling_.peer_address(signaling_peer, signaling_peer_len)) {
                    std::memcpy(&mc.peer_rtcp_addr, &signaling_peer, sizeof(signaling_peer));
                    mc.peer_rtcp_addr_len = signaling_peer_len;
                    if (signaling_peer.ss_family == AF_INET) {
                        reinterpret_cast<sockaddr_in*>(&mc.peer_rtcp_addr)->sin_port =
                            ::htons(result.server_rtcp_port);
                    } else if (signaling_peer.ss_family == AF_INET6) {
                        reinterpret_cast<sockaddr_in6*>(&mc.peer_rtcp_addr)->sin6_port =
                            ::htons(result.server_rtcp_port);
                    }
                    mc.peer_rtcp_set = true;
                    MCP_LOGF(pal::LogLevel::info,
                             "TsRtspUdp: %s RTCP peer primed via SETUP server_port=%u "
                             "(family=%d) — first PLI 可立刻发,无需等 server SR",
                             m.kind == SdpMedia::Kind::video ? "video" : "audio",
                             static_cast<unsigned>(result.server_rtcp_port),
                             static_cast<int>(signaling_peer.ss_family));
                }
            }

            channels_.push_back(std::move(mc));
            sdp_ = sdp;
        }

        if (channels_.empty()) {
            close();
            return MC_ERR_PROTOCOL;
        }

        if (mc_status_t s = client_.do_play(base_url_, channels_.front().session_id,
                                              connect_timeout_ms);
            s != MC_OK) {
            close();
            return s;
        }

        // 通知 sink stream 准备就绪
        if (sink_.on_stream_ready) {
            StreamDescription desc;
            desc.sdp      = sdp_;
            desc.base_uri = base_url_;
            sink_.on_stream_ready(desc);
        }

        keepalive_ms_ = keepalive_interval_ms != 0
            ? keepalive_interval_ms
            : std::max<uint32_t>(kDefaultKeepaliveMs,
                                  channels_.front().timeout_seconds * 1000u / kKeepaliveDivisor);

        running_.store(true, std::memory_order_release);
        rx_thread_       = std::thread([this] { run_rx_loop(); });
        keepalive_thread_= std::thread([this] { run_keepalive_loop(); });
        return MC_OK;
    }

    void close() noexcept override {
        if (running_.exchange(false, std::memory_order_acq_rel)) {
            if (!channels_.empty()) {
                client_.do_teardown(base_url_, channels_.front().session_id);
            }
            // RX / keepalive 线程会因 socket close + flag 退出
            for (auto& c : channels_) {
                c.rtp.close();
                c.rtcp.close();
            }
            if (rx_thread_.joinable())        rx_thread_.join();
            if (keepalive_thread_.joinable()) keepalive_thread_.join();
            channels_.clear();
            signaling_.close();
        }
    }

    mc_status_t send_rtcp(MediaKind kind, std::span<const uint8_t> bytes) noexcept override {
        for (auto& c : channels_) {
            if ((kind == MediaKind::video && c.kind == SdpMedia::Kind::video) ||
                (kind == MediaKind::audio && c.kind == SdpMedia::Kind::audio)) {
                if (!c.peer_rtcp_set) return MC_ERR_INVALID_STATE;
                return c.rtcp.send_to(bytes,
                                      reinterpret_cast<const sockaddr*>(&c.peer_rtcp_addr),
                                      c.peer_rtcp_addr_len);
            }
        }
        return MC_ERR_INVALID_STATE;
    }

private:
    struct MediaChannel {
        SdpMedia::Kind   kind;
        std::string      session_id;
        uint32_t         timeout_seconds = 60;

        pal::UdpSocket   rtp;
        pal::UdpSocket   rtcp;

        sockaddr_in6     peer_rtcp_addr{};
        int              peer_rtcp_addr_len = 0;
        bool             peer_rtcp_set      = false;
    };

    void run_rx_loop() noexcept {
        pal::ThreadRegistration reg;
        pal::ThreadOptions opt;
        opt.name          = "mc-player T2 RTSP-RX";
        opt.base_priority = THREAD_PRIORITY_TIME_CRITICAL;
        opt.affinity      = pal::ThreadAffinityHint::pcore_only;
        reg.apply(opt);

        // 简化版：阻塞 recvfrom 各 socket 各自一线程会更直接，但用统一 select 减少线程数。
        // RFC 一致性 & 低延迟同时要求：v1 用 select 多路复用；后续可换 IOCP 共享端口。
        while (running_.load(std::memory_order_acquire)) {
            fd_set rds;
            FD_ZERO(&rds);
            SOCKET maxfd = 0;
            for (auto& c : channels_) {
                if (c.rtp.valid())  { FD_SET(c.rtp.native(), &rds);  if (c.rtp.native()  > maxfd) maxfd = c.rtp.native(); }
                if (c.rtcp.valid()) { FD_SET(c.rtcp.native(), &rds); if (c.rtcp.native() > maxfd) maxfd = c.rtcp.native(); }
            }
            timeval tv{0, 100'000};   // 100 ms
            int rc = ::select(static_cast<int>(maxfd) + 1, &rds, nullptr, nullptr, &tv);
            if (rc <= 0) continue;

            for (auto& c : channels_) {
                if (c.rtp.valid() && FD_ISSET(c.rtp.native(), &rds)) {
                    drain_rtp(c);
                }
                if (c.rtcp.valid() && FD_ISSET(c.rtcp.native(), &rds)) {
                    drain_rtcp(c);
                }
            }
        }
    }

    void drain_rtp(MediaChannel& c) noexcept {
        std::vector<uint8_t> buf(kRxBufferBytes);
        for (;;) {
            sockaddr_in6 from{};
            int          from_len = sizeof(from);
            int n = ::recvfrom(c.rtp.native(), reinterpret_cast<char*>(buf.data()),
                                static_cast<int>(buf.size()), 0,
                                reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n <= 0) break;
            if (sink_.on_rtp) {
                RtpDatagram dg;
                dg.kind           = c.kind == SdpMedia::Kind::video ? MediaKind::video : MediaKind::audio;
                dg.arrival_qpc_ns = pal::Clock::now_ns();
                dg.bytes.assign(buf.begin(), buf.begin() + n);
                sink_.on_rtp(dg);
            }
        }
    }

    void drain_rtcp(MediaChannel& c) noexcept {
        std::vector<uint8_t> buf(kRxBufferBytes);
        for (;;) {
            sockaddr_in6 from{};
            int          from_len = sizeof(from);
            int n = ::recvfrom(c.rtcp.native(), reinterpret_cast<char*>(buf.data()),
                                static_cast<int>(buf.size()), 0,
                                reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n <= 0) break;
            if (!c.peer_rtcp_set) {
                std::memcpy(&c.peer_rtcp_addr, &from, sizeof(from));
                c.peer_rtcp_addr_len = from_len;
                c.peer_rtcp_set      = true;
            }
            if (sink_.on_rtcp) {
                std::vector<RtcpSenderReport>     srs;
                std::vector<RtcpReceiverReport>   rrs;
                std::vector<RtcpFeedbackNack>     nacks;
                std::vector<RtcpFeedbackPli>      plis;
                RtcpReader reader;
                if (reader.parse(std::span<const uint8_t>(buf.data(), n), srs, rrs, nacks, plis)) {
                    sink_.on_rtcp(srs, rrs, nacks, plis);
                }
            }
        }
    }

    void run_keepalive_loop() noexcept {
        while (running_.load(std::memory_order_acquire)) {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(keepalive_ms_);
            while (running_.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!running_.load(std::memory_order_acquire)) break;
            if (!channels_.empty()) {
                (void)client_.do_keepalive(base_url_, channels_.front().session_id, 5'000);
            }
        }
    }

    std::mutex                 mu_;
    TransportSessionSink       sink_;

    TcpStream                  signaling_;
    RtspClient                 client_;
    std::string                base_url_;
    SdpSession                 sdp_;
    std::vector<MediaChannel>  channels_;

    std::atomic<bool>          running_{false};
    std::thread                rx_thread_;
    std::thread                keepalive_thread_;
    uint32_t                   keepalive_ms_{kDefaultKeepaliveMs};
};

}  // namespace

std::unique_ptr<TransportSession> make_ts_rtsp_udp() {
    return std::make_unique<TsRtspUdp>();
}

std::unique_ptr<TransportSession> make_rtsp_transport(mc_protocol_t hint, bool /*allow_tcp_fallback*/) {
    // hint = TCP（interleaved）时直接走 TCP；UDP / AUTO 走 UDP（部分摄像头 UDP 模式不发 audio，
    // caller 需切 hint=TCP 兜底）。controller 之后做 UDP→TCP 自动 fallback。
    if (hint == MC_PROTOCOL_RTSP_TCP) {
        return make_ts_rtsp_tcp();
    }
    return make_ts_rtsp_udp();
}

}  // namespace mcp::transport
