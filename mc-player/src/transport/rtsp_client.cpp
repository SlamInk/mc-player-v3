#include "transport/rtsp_client.h"

#include <atomic>
#include <chrono>
#include <random>
#include <sstream>

#include "pal/log.h"

namespace mcp::transport {

namespace {

constexpr uint32_t kDefaultRtspTimeoutMs = 5000;

std::string make_cnonce() noexcept {
    std::random_device rd;
    std::mt19937_64    gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(dist(gen)));
    return buf;
}

std::string format_nc(uint32_t nc) noexcept {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", nc);
    return buf;
}

std::string parse_session_header(std::string_view value, uint32_t* timeout_seconds) noexcept {
    std::string id;
    auto semi = value.find(';');
    if (semi == std::string_view::npos) {
        id.assign(value);
        return id;
    }
    id.assign(value.substr(0, semi));
    auto rest = value.substr(semi + 1);
    auto pos = rest.find("timeout=");
    if (pos != std::string_view::npos && timeout_seconds) {
        rest.remove_prefix(pos + 8);
        uint32_t t = 0;
        std::from_chars(rest.data(), rest.data() + rest.size(), t);
        if (t > 0) *timeout_seconds = t;
    }
    return id;
}

}  // namespace

struct RtspClient::Impl {
    std::string             username;
    std::string             password;
    RtspByteIo              io;
    RtspParser              parser;

    uint32_t                cseq{1};
    uint32_t                nc{1};
    DigestChallenge         challenge;
    bool                    has_challenge{false};
    std::string             cnonce;

    mc_status_t round_trip(RtspRequest& req,
                            uint32_t timeout_ms,
                            RtspResponse& resp) noexcept {
        if (!io.send || !io.read_some) {
            return MC_ERR_INVALID_STATE;
        }

        req.cseq = ++cseq;
        // 已有挑战时附 Authorization。
        if (has_challenge && !username.empty()) {
            if (cnonce.empty()) cnonce = make_cnonce();
            std::string nc_str = format_nc(nc++);
            std::string auth = build_digest_authorization(
                challenge, username, password, req.method, req.uri, cnonce, nc_str);
            if (!auth.empty()) {
                req.set_header("Authorization", std::move(auth));
            }
        }
        std::string serialized = req.serialize();
        MCP_LOGF(pal::LogLevel::info,
                 "RTSP TX cseq=%u method=%s uri=%s bytes=%zu",
                 req.cseq, req.method.c_str(), req.uri.c_str(), serialized.size());
        if (!io.send(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(serialized.data()),
                                               serialized.size()))) {
            MCP_LOG_WARN("RTSP TX send failed (socket error)");
            return MC_ERR_IO;
        }

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        std::vector<uint8_t> chunk;
        std::size_t total_recv = 0;
        while (true) {
            chunk.clear();
            if (!io.read_some(chunk)) {
                MCP_LOGF(pal::LogLevel::warn,
                         "RTSP RX read_some failed cseq=%u total_bytes_before_fail=%zu "
                         "(socket closed by peer or timeout)",
                         req.cseq, total_recv);
                return MC_ERR_IO;
            }
            total_recv += chunk.size();
            if (!chunk.empty()) {
                parser.append(std::string_view{
                    reinterpret_cast<const char*>(chunk.data()), chunk.size()});
            }
            if (auto r = parser.next_response()) {
                resp = std::move(*r);
                MCP_LOGF(pal::LogLevel::info,
                         "RTSP RX cseq=%u status=%d (req cseq=%u, body_bytes=%zu)",
                         resp.cseq, resp.status_code, req.cseq, resp.body.size());
                if (resp.cseq == req.cseq) {
                    return MC_OK;
                }
                // CSeq 不匹配：服务器也许把另一会话的响应混在；丢弃后继续等。
                continue;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                MCP_LOGF(pal::LogLevel::warn,
                         "RTSP RX timeout cseq=%u total_bytes=%zu",
                         req.cseq, total_recv);
                return MC_ERR_TIMEOUT;
            }
        }
    }

    mc_status_t request_with_auth(RtspRequest req,
                                   uint32_t timeout_ms,
                                   RtspResponse& resp) noexcept {
        for (int attempt = 0; attempt < 2; ++attempt) {
            mc_status_t s = round_trip(req, timeout_ms, resp);
            if (s != MC_OK) return s;
            if (resp.status_code != 401) return MC_OK;
            if (attempt == 1) return MC_ERR_AUTH;

            const std::string* www_auth = resp.find_header("WWW-Authenticate");
            if (!www_auth) return MC_ERR_AUTH;
            auto ch = parse_digest_challenge(*www_auth);
            if (!ch) return MC_ERR_AUTH;
            challenge     = std::move(*ch);
            has_challenge = true;
            nc            = 1;
            cnonce.clear();
            // 构造同一请求二次发送（cseq 在 round_trip 内 +1）；不需重新构造。
        }
        return MC_ERR_AUTH;
    }
};

RtspClient::RtspClient()  : impl_{std::make_unique<Impl>()} {}
RtspClient::~RtspClient() = default;

void RtspClient::set_credentials(std::string username, std::string password) noexcept {
    impl_->username = std::move(username);
    impl_->password = std::move(password);
}

void RtspClient::set_byte_io(RtspByteIo io) noexcept {
    impl_->io = std::move(io);
}

mc_status_t RtspClient::do_describe(const std::string& url,
                                     uint32_t timeout_ms,
                                     SdpSession& out_sdp) noexcept {
    if (timeout_ms == 0) timeout_ms = kDefaultRtspTimeoutMs;

    // OPTIONS — 让 server 暴露能力，并触发 401（如果需要认证）。
    {
        RtspRequest req;
        req.method = "OPTIONS";
        req.uri    = url;
        req.set_header("User-Agent", "mc-player/0.1");
        RtspResponse resp;
        if (mc_status_t s = impl_->request_with_auth(req, timeout_ms, resp); s != MC_OK) {
            return s;
        }
        if (resp.status_code / 100 != 2) {
            MCP_LOGF(pal::LogLevel::warn,
                     "RTSP OPTIONS failed: %d %s", resp.status_code, resp.reason.c_str());
            return MC_ERR_PROTOCOL;
        }
    }

    // DESCRIBE
    RtspRequest req;
    req.method = "DESCRIBE";
    req.uri    = url;
    req.set_header("User-Agent", "mc-player/0.1");
    req.set_header("Accept", "application/sdp");

    RtspResponse resp;
    if (mc_status_t s = impl_->request_with_auth(req, timeout_ms, resp); s != MC_OK) {
        return s;
    }
    if (resp.status_code / 100 != 2) {
        return MC_ERR_PROTOCOL;
    }
    auto parsed = SdpParser::parse(resp.body);
    if (!parsed) return MC_ERR_PROTOCOL;
    out_sdp = std::move(*parsed);
    return MC_OK;
}

mc_status_t RtspClient::do_setup(const std::string& base_url,
                                  const RtspSetupTarget& target,
                                  uint32_t timeout_ms,
                                  RtspSetupResult& out_result) noexcept {
    if (timeout_ms == 0) timeout_ms = kDefaultRtspTimeoutMs;

    // 拼 SETUP URI：control_uri 可能是相对的。RFC 2326 §C.1 推荐相对解析。
    std::string setup_uri;
    if (target.control_uri.empty() || target.control_uri == "*") {
        setup_uri = base_url;
    } else if (target.control_uri.find("://") != std::string::npos) {
        setup_uri = target.control_uri;
    } else {
        setup_uri = base_url;
        if (setup_uri.back() != '/' && target.control_uri.front() != '/') {
            setup_uri.push_back('/');
        }
        setup_uri.append(target.control_uri);
    }

    RtspRequest req;
    req.method = "SETUP";
    req.uri    = setup_uri;
    req.set_header("User-Agent", "mc-player/0.1");

    std::string transport_hdr;
    if (target.interleaved) {
        std::ostringstream os;
        os << "RTP/AVP/TCP;unicast;interleaved="
           << static_cast<int>(target.interleaved_rtp_channel) << "-"
           << static_cast<int>(target.interleaved_rtcp_channel);
        transport_hdr = os.str();
    } else {
        std::ostringstream os;
        os << "RTP/AVP;unicast;client_port="
           << target.client_rtp_port << "-" << target.client_rtcp_port;
        transport_hdr = os.str();
    }
    req.set_header("Transport", std::move(transport_hdr));
    if (!target.existing_session_id.empty()) {
        req.set_header("Session", target.existing_session_id);
    }

    RtspResponse resp;
    if (mc_status_t s = impl_->request_with_auth(req, timeout_ms, resp); s != MC_OK) {
        return s;
    }
    if (resp.status_code == 461) {
        return MC_ERR_UNSUPPORTED;     // Unsupported transport — caller 决定降级
    }
    if (resp.status_code / 100 != 2) return MC_ERR_PROTOCOL;

    if (auto* sess = resp.find_header("Session"); sess != nullptr) {
        out_result.session_id = parse_session_header(*sess, &out_result.timeout_seconds);
    } else {
        return MC_ERR_PROTOCOL;
    }
    out_result.kind = target.kind;

    if (auto* tr = resp.find_header("Transport"); tr != nullptr) {
        // 简化解析：只取 server_port=A-B 与 ssrc=
        auto pos = tr->find("server_port=");
        if (pos != std::string::npos) {
            std::string_view rest{*tr};
            rest.remove_prefix(pos + std::string_view{"server_port="}.size());
            uint32_t a = 0, b = 0;
            std::from_chars(rest.data(), rest.data() + rest.size(), a);
            auto dash = rest.find('-');
            if (dash != std::string_view::npos) {
                rest.remove_prefix(dash + 1);
                std::from_chars(rest.data(), rest.data() + rest.size(), b);
            }
            out_result.server_rtp_port  = static_cast<uint16_t>(a & 0xFFFF);
            out_result.server_rtcp_port = static_cast<uint16_t>(b & 0xFFFF);
        }
        if (auto sp = tr->find("ssrc="); sp != std::string::npos) {
            std::string_view rest{*tr};
            rest.remove_prefix(sp + std::string_view{"ssrc="}.size());
            uint32_t v = 0;
            std::from_chars(rest.data(), rest.data() + rest.size(), v, 16);
            out_result.ssrc = v;
        }
    }
    return MC_OK;
}

mc_status_t RtspClient::do_play(const std::string& base_url,
                                 const std::string& session_id,
                                 uint32_t timeout_ms) noexcept {
    if (timeout_ms == 0) timeout_ms = kDefaultRtspTimeoutMs;
    RtspRequest req;
    req.method = "PLAY";
    req.uri    = base_url;
    req.set_header("Session", session_id);
    req.set_header("Range",   "npt=0.000-");
    req.set_header("User-Agent", "mc-player/0.1");
    RtspResponse resp;
    if (mc_status_t s = impl_->request_with_auth(req, timeout_ms, resp); s != MC_OK) return s;
    return resp.status_code / 100 == 2 ? MC_OK : MC_ERR_PROTOCOL;
}

mc_status_t RtspClient::do_keepalive(const std::string& base_url,
                                      const std::string& session_id,
                                      uint32_t timeout_ms) noexcept {
    if (timeout_ms == 0) timeout_ms = kDefaultRtspTimeoutMs;
    RtspRequest req;
    req.method = "GET_PARAMETER";
    req.uri    = base_url;
    req.set_header("Session", session_id);
    req.set_header("User-Agent", "mc-player/0.1");
    RtspResponse resp;
    mc_status_t s = impl_->request_with_auth(req, timeout_ms, resp);
    if (s == MC_OK && resp.status_code / 100 != 2) {
        // 部分服务器拒绝 GET_PARAMETER；回退 OPTIONS。
        RtspRequest req2;
        req2.method = "OPTIONS";
        req2.uri    = base_url;
        req2.set_header("Session", session_id);
        req2.set_header("User-Agent", "mc-player/0.1");
        s = impl_->request_with_auth(req2, timeout_ms, resp);
        if (s != MC_OK) return s;
        return resp.status_code / 100 == 2 ? MC_OK : MC_ERR_PROTOCOL;
    }
    return s;
}

void RtspClient::do_teardown(const std::string& base_url,
                              const std::string& session_id) noexcept {
    RtspRequest req;
    req.method = "TEARDOWN";
    req.uri    = base_url;
    req.set_header("Session", session_id);
    req.set_header("User-Agent", "mc-player/0.1");
    RtspResponse resp;
    (void)impl_->request_with_auth(req, kDefaultRtspTimeoutMs, resp);
    if (impl_->io.shutdown) impl_->io.shutdown();
}

}  // namespace mcp::transport
