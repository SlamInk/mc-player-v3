/*
 * RTSP TCP interleaved transport（RFC 2326 §10.12）。
 *
 * 单 TCP socket 既走 RTSP 信令，又承载 RTP/RTCP interleaved 帧：
 *   ASCII 起头 → RTSP 文本（响应 / SERVER 推送）
 *   '$' 起头   → 4 字节头（'$' + 1B channel + 2B length BE）+ payload
 *
 * 部分摄像头（含海康常见配置）在 UDP 模式下不稳定地推 audio；TCP interleaved 是
 * 现网兜底路径，VLC 默认行为也优先尝试 TCP。
 *
 * 线程模型：
 *   RX 线程：阻塞 recv → demuxer 拆分 → RTSP 文本入 inbox（cv 通知）/ 数据帧 fan-out 到 sink
 *   主线程（caller of open()）：用 RtspByteIo 同步驱动 DESCRIBE/SETUP/PLAY；
 *                              read_some 从 inbox 阻塞取；send 直接 socket send（mutex 保护）
 *   keepalive 线程：周期性 GET_PARAMETER（与 main 共用 socket，send 走 mutex）
 */

#include "transport/ts_rtsp_tcp.h"

#include <Windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "pal/clock.h"
#include "pal/error.h"
#include "pal/log.h"
#include "pal/raii.h"
#include "pal/thread.h"
#include "transport/rtsp_client.h"
#include "transport/sdp_parser.h"
#include "transport/rtcp.h"

namespace mcp::transport {

namespace {

constexpr uint8_t  kInterleavedSentinel = '$';
constexpr uint32_t kDefaultKeepaliveMs  = 25'000;
constexpr int      kKeepaliveDivisor    = 2;

struct ParsedUrl {
    std::string scheme;
    std::string user, pass, host, path;
    uint16_t    port = 0;
};

bool parse_rtsp_url(std::string_view url, ParsedUrl& out) noexcept {
    // 与 ts_rtsp_udp.cpp::parse_rtsp_url 保持一致:trim ASCII 空白,
    // 防御 caller 传入含尾部空格的 URL 触发 SETUP 拼接畸形 URI(详见 udp 版注释)。
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
    std::string_view authority = (path_pos == std::string_view::npos)
                                  ? rest : rest.substr(0, path_pos);
    out.path.assign(path_pos == std::string_view::npos ? std::string_view{"/"} : rest.substr(path_pos));
    auto at = authority.find('@');
    if (at != std::string_view::npos) {
        auto creds = authority.substr(0, at);
        authority  = authority.substr(at + 1);
        auto colon = creds.find(':');
        if (colon != std::string_view::npos) {
            out.user.assign(creds.substr(0, colon));
            out.pass.assign(creds.substr(colon + 1));
        } else { out.user.assign(creds); }
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

class TsRtspTcp final : public TransportSession {
public:
    TsRtspTcp() = default;
    ~TsRtspTcp() override { close(); }

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
        if (u.scheme != "rtsp" && u.scheme != "rtsps") return MC_ERR_UNSUPPORTED;

        const std::string user = !username.empty() ? username : u.user;
        const std::string pass = !password.empty() ? password : u.pass;

        if (mc_status_t s = connect_socket(u.host, u.port, connect_timeout_ms); s != MC_OK) {
            return s;
        }

        // 重组无 user:pass 的 base URL。
        std::string base_url = u.scheme + "://" + u.host;
        if ((u.scheme == "rtsp"  && u.port != 554) ||
            (u.scheme == "rtsps" && u.port != 322)) {
            base_url += ":" + std::to_string(u.port);
        }
        base_url += u.path;
        base_url_ = base_url;

        client_.set_credentials(user, pass);

        // RtspByteIo 桥到本类的 send / inbox 阻塞读。
        RtspByteIo io;
        io.send       = [this](std::span<const uint8_t> b) { return tcp_send(b); };
        io.read_some  = [this](std::vector<uint8_t>& out) { return rtsp_read_some(out); };
        io.shutdown   = [this] { close_socket(); };
        client_.set_byte_io(std::move(io));

        // RX 线程必须在发出第一个 RTSP 请求之前启动 — RtspByteIo.read_some 等它入队。
        running_.store(true, std::memory_order_release);
        rx_thread_ = std::thread([this] { run_rx_loop(); });

        SdpSession sdp;
        if (mc_status_t s = client_.do_describe(base_url_, connect_timeout_ms, sdp); s != MC_OK) {
            close_unlocked();
            return s;
        }

        // SETUP 每个 track；用 interleaved channel 对：video 0/1, audio 2/3 ...
        uint8_t next_ch = 0;
        for (auto& m : sdp.media) {
            if (m.kind != SdpMedia::Kind::video && m.kind != SdpMedia::Kind::audio) continue;

            RtspSetupTarget tgt;
            tgt.kind                     = m.kind;
            tgt.control_uri              = m.control_uri;
            tgt.interleaved              = true;
            tgt.interleaved_rtp_channel  = next_ch;
            tgt.interleaved_rtcp_channel = static_cast<uint8_t>(next_ch + 1);
            if (!session_id_.empty()) tgt.existing_session_id = session_id_;

            RtspSetupResult result;
            if (mc_status_t s = client_.do_setup(base_url_, tgt, connect_timeout_ms, result); s != MC_OK) {
                MCP_LOGF(pal::LogLevel::warn,
                         "TsRtspTcp: SETUP %s failed status=%d",
                         m.kind == SdpMedia::Kind::video ? "video" : "audio",
                         static_cast<int>(s));
                close_unlocked();
                return s;
            }
            session_id_      = result.session_id;
            timeout_seconds_ = result.timeout_seconds;

            ChannelInfo ci_rtp{m.kind, false};
            ChannelInfo ci_rtcp{m.kind, true};
            channel_map_[next_ch]                            = ci_rtp;
            channel_map_[static_cast<uint8_t>(next_ch + 1)]  = ci_rtcp;
            channel_kinds_.push_back(m.kind);

            // 缓存 RTCP 上行 channel：上层 send_rtcp(kind, bytes) 不需感知 SDP 顺序。
            const MediaKind mk = (m.kind == SdpMedia::Kind::video)
                                    ? MediaKind::video : MediaKind::audio;
            rtcp_ch_by_kind_[mk] = static_cast<uint8_t>(next_ch + 1);
            MCP_LOGF(pal::LogLevel::info,
                     "TsRtspTcp: SETUP %s ok, interleaved ch=%u/%u session=%s",
                     m.kind == SdpMedia::Kind::video ? "video" : "audio",
                     next_ch, next_ch + 1, session_id_.c_str());

            next_ch = static_cast<uint8_t>(next_ch + 2);
        }

        if (channel_kinds_.empty()) {
            close_unlocked();
            return MC_ERR_PROTOCOL;
        }

        // PLAY 之前先发布 channel_map(release 同步),防止 server 在 PLAY 前发 interleaved
        // 触发 unordered_map concurrent emplace+find race。channel_map_ 自此 read-only。
        channels_published_.store(true, std::memory_order_release);

        if (mc_status_t s = client_.do_play(base_url_, session_id_, connect_timeout_ms); s != MC_OK) {
            close_unlocked();
            return s;
        }

        sdp_ = sdp;
        if (sink_.on_stream_ready) {
            StreamDescription desc;
            desc.sdp      = sdp_;
            desc.base_uri = base_url_;
            sink_.on_stream_ready(desc);
        }

        keepalive_ms_ = keepalive_interval_ms != 0
            ? keepalive_interval_ms
            : std::max<uint32_t>(kDefaultKeepaliveMs,
                                  timeout_seconds_ * 1000u / kKeepaliveDivisor);
        keepalive_thread_ = std::thread([this] { run_keepalive_loop(); });
        return MC_OK;
    }

    void close() noexcept override {
        std::scoped_lock lk{mu_};
        close_unlocked();
    }

    mc_status_t send_rtcp(MediaKind kind, std::span<const uint8_t> bytes) noexcept override {
        if (bytes.empty() || bytes.size() > 0xFFFF) return MC_ERR_INVALID_ARG;
        if (!running_.load(std::memory_order_acquire)) return MC_ERR_INVALID_STATE;
        // 与 dispatch_interleaved 同样需要 acquire 同步,否则 SETUP 期间用户调 send_rtcp
        // 与 line 192 的 emplace 并发 → unordered_map UB。
        if (!channels_published_.load(std::memory_order_acquire)) return MC_ERR_INVALID_STATE;
        uint8_t ch;
        {
            auto it = rtcp_ch_by_kind_.find(kind);
            if (it == rtcp_ch_by_kind_.end()) return MC_ERR_UNSUPPORTED;
            ch = it->second;
        }
        // RFC 2326 §10.12 interleaved 帧：'$' + 1B channel + 2B BE length + payload。
        std::vector<uint8_t> frame(4 + bytes.size());
        frame[0] = kInterleavedSentinel;
        frame[1] = ch;
        frame[2] = static_cast<uint8_t>((bytes.size() >> 8) & 0xFF);
        frame[3] = static_cast<uint8_t>(bytes.size() & 0xFF);
        std::memcpy(frame.data() + 4, bytes.data(), bytes.size());
        return tcp_send(frame) ? MC_OK : MC_ERR_IO;
    }

private:
    struct ChannelInfo {
        SdpMedia::Kind kind;
        bool           is_rtcp;
    };

    mc_status_t connect_socket(const std::string& host, uint16_t port, uint32_t timeout_ms) noexcept {
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
            DWORD tv = timeout_ms;
            ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                          reinterpret_cast<const char*>(&tv), sizeof(tv));
            ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
                          reinterpret_cast<const char*>(&tv), sizeof(tv));
            if (::connect(s, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) break;
            ::closesocket(s);
            s = INVALID_SOCKET;
        }
        if (s == INVALID_SOCKET) return MC_ERR_IO;

        // 数据流之后取消 recv 超时（让 recv 阻塞），但保留 send 超时防 socket 死锁。
        DWORD zero = 0;
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                      reinterpret_cast<const char*>(&zero), sizeof(zero));
        // TCP_NODELAY：信令 + 流低延时。
        BOOL nodelay = TRUE;
        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                      reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
        sock_.reset(s);
        return MC_OK;
    }

    void close_socket() noexcept {
        SOCKET s = sock_.release();
        if (s != INVALID_SOCKET) {
            ::shutdown(s, SD_BOTH);
            ::closesocket(s);
        }
    }

    void close_unlocked() noexcept {
        if (running_.exchange(false, std::memory_order_acq_rel)) {
            if (!session_id_.empty()) {
                (void)client_.do_teardown(base_url_, session_id_);
            }
        }
        // RX 线程因为 socket 被关闭 + running=false 自然退出。
        close_socket();
        recv_cv_.notify_all();
        if (rx_thread_.joinable())        rx_thread_.join();
        if (keepalive_thread_.joinable()) keepalive_thread_.join();
        channel_map_.clear();
        channel_kinds_.clear();
        session_id_.clear();
    }

    bool tcp_send(std::span<const uint8_t> bytes) noexcept {
        std::scoped_lock lk{send_mu_};
        if (!sock_.valid()) return false;
        const uint8_t* p = bytes.data();
        std::size_t n = bytes.size();
        while (n > 0) {
            int sent = ::send(sock_.get(), reinterpret_cast<const char*>(p),
                               static_cast<int>(std::min<std::size_t>(n, INT_MAX)), 0);
            if (sent <= 0) return false;
            p += sent;
            n -= static_cast<std::size_t>(sent);
        }
        return true;
    }

    bool rtsp_read_some(std::vector<uint8_t>& out) noexcept {
        std::unique_lock lk{recv_mu_};
        recv_cv_.wait(lk, [this] {
            return !rtsp_inbox_.empty() || !running_.load(std::memory_order_acquire);
        });
        if (rtsp_inbox_.empty()) return false;
        out.assign(rtsp_inbox_.begin(), rtsp_inbox_.end());
        rtsp_inbox_.clear();
        return true;
    }

    void run_rx_loop() noexcept {
        // T2 Network RX (ADD §3.3)：TIME_CRITICAL + P-core 绑定。TCP 路径与 UDP 同等关键
        // ——RTP/RTCP interleaved 帧也必须低 jitter 接收，否则进入 jitter buffer 的到达时间
        // 抖动直接污染 Kalman 估计。
        pal::ThreadRegistration reg;
        pal::ThreadOptions opt;
        opt.name          = "mc-player T2 RTSP-TCP-RX";
        opt.base_priority = THREAD_PRIORITY_TIME_CRITICAL;
        opt.affinity      = pal::ThreadAffinityHint::pcore_only;
        reg.apply(opt);

        std::vector<uint8_t> buf(64 * 1024);
        // demux_buf 用 read offset 替代每帧 erase from front：burst 关键帧 100KB+ 时
        // 原 erase 触发整段 memmove ~100μs/次。read_pos 只在 buffer 已全消费 或
        // 偏移过半时整体 compact，把 memmove 频率从每帧 1 次降到每 N 帧 1 次。
        std::vector<uint8_t> demux_buf;
        demux_buf.reserve(128 * 1024);
        std::size_t read_pos = 0;
        constexpr std::size_t kCompactThreshold = 32 * 1024;

        while (running_.load(std::memory_order_acquire)) {
            int n = ::recv(sock_.get(), reinterpret_cast<char*>(buf.data()),
                            static_cast<int>(buf.size()), 0);
            if (n <= 0) {
                running_.store(false, std::memory_order_release);
                recv_cv_.notify_all();
                if (sink_.on_error) {
                    sink_.on_error(MC_ERR_IO, "TsRtspTcp: socket closed by peer");
                }
                break;
            }
            // recv 入站：append 到尾部；若缓冲已被消费过半则先 compact 防止无限增长。
            if (read_pos > kCompactThreshold) {
                demux_buf.erase(demux_buf.begin(), demux_buf.begin() + read_pos);
                read_pos = 0;
            }
            demux_buf.insert(demux_buf.end(), buf.begin(), buf.begin() + n);

            // 拆 demux_buf：按规则取一段 RTSP 文本或一帧 interleaved。
            for (;;) {
                const std::size_t avail = demux_buf.size() - read_pos;
                if (avail == 0) break;
                const uint8_t* p = demux_buf.data() + read_pos;
                if (p[0] != kInterleavedSentinel) {
                    // 找下一个 '$' 之前的所有字节作为 RTSP 文本块。
                    std::size_t end = 0;
                    while (end < avail && p[end] != kInterleavedSentinel) ++end;
                    {
                        std::scoped_lock lk{recv_mu_};
                        rtsp_inbox_.insert(rtsp_inbox_.end(), p, p + end);
                    }
                    recv_cv_.notify_all();
                    read_pos += end;
                    if (read_pos == demux_buf.size()) break;
                    // 余下以 '$' 起头，进入下一轮处理。
                    continue;
                }
                // interleaved 帧：'$' + ch(1) + len(2 BE) + payload(len)
                if (avail < 4) break;
                const uint8_t  ch  = p[1];
                const uint16_t len = static_cast<uint16_t>(
                    (static_cast<uint16_t>(p[2]) << 8) | p[3]);
                if (avail < static_cast<std::size_t>(4) + len) break;

                dispatch_interleaved(ch, std::span<const uint8_t>(p + 4, len));
                read_pos += static_cast<std::size_t>(4) + len;
            }
            // 缓冲全部消费完时直接 reset，避免 vector 持续增长。
            if (read_pos == demux_buf.size()) {
                demux_buf.clear();
                read_pos = 0;
            }
        }
    }

    void dispatch_interleaved(uint8_t ch, std::span<const uint8_t> payload) noexcept {
        // 在 channel_map_ 完全发布前(SETUP 阶段)直接 drop,与 channels_published_.store
        // release 配对,确保读侧看到完整 map 写入结果而不是半态 hashtable。
        if (!channels_published_.load(std::memory_order_acquire)) return;
        auto it = channel_map_.find(ch);
        if (it == channel_map_.end()) return;
        if (!it->second.is_rtcp) {
            if (sink_.on_rtp) {
                RtpDatagram dg;
                dg.kind           = it->second.kind == SdpMedia::Kind::video
                                      ? MediaKind::video : MediaKind::audio;
                dg.arrival_qpc_ns = pal::Clock::now_ns();
                dg.bytes.assign(payload.begin(), payload.end());
                sink_.on_rtp(dg);
            }
        } else if (sink_.on_rtcp) {
            std::vector<RtcpSenderReport>     srs;
            std::vector<RtcpReceiverReport>   rrs;
            std::vector<RtcpFeedbackNack>     nacks;
            std::vector<RtcpFeedbackPli>      plis;
            RtcpReader reader;
            if (reader.parse(payload, srs, rrs, nacks, plis)) {
                sink_.on_rtcp(srs, rrs, nacks, plis);
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
            if (!session_id_.empty()) {
                (void)client_.do_keepalive(base_url_, session_id_, 5'000);
            }
        }
    }

    // socket / send 同步
    pal::SocketGuard           sock_;
    std::mutex                 send_mu_;

    // RX 线程 → main / RtspByteIo.read_some 之间的 inbox
    std::mutex                  recv_mu_;
    std::condition_variable     recv_cv_;
    std::deque<uint8_t>         rtsp_inbox_;

    // 本类生命周期
    std::mutex                  mu_;
    TransportSessionSink        sink_;
    std::atomic<bool>           running_{false};
    std::thread                 rx_thread_;
    std::thread                 keepalive_thread_;

    // RTSP 元数据
    RtspClient                  client_;
    SdpSession                  sdp_;
    std::string                 base_url_;
    std::string                 session_id_;
    uint32_t                    timeout_seconds_ = 60;
    uint32_t                    keepalive_ms_    = kDefaultKeepaliveMs;

    // interleaved channel → media 路由
    // RX 线程在 SETUP 期间已 running,但 channel_map_ 由用户线程在 SETUP 后才填(line 185)。
    // PLAY 200 OK 之前 server 不应发 interleaved 帧,但畸形流仍可能 race;
    // channels_published_ 给 RX 一道 happens-before 同步点(release 写,acquire 读)
    // 让 dispatch_interleaved 在写入完成前直接 drop,杜绝 unordered_map 半态读。
    std::unordered_map<uint8_t, ChannelInfo> channel_map_;
    std::vector<SdpMedia::Kind>              channel_kinds_;
    // 出站 RTCP 路由：MediaKind → interleaved channel
    std::unordered_map<MediaKind, uint8_t>   rtcp_ch_by_kind_;
    std::atomic<bool>                        channels_published_{false};
};

}  // namespace

std::unique_ptr<TransportSession> make_ts_rtsp_tcp() {
    return std::make_unique<TsRtspTcp>();
}

}  // namespace mcp::transport
