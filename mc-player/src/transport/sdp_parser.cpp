#include "transport/sdp_parser.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>
#include <string_view>

namespace mcp::transport {

namespace {

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t' || s.back()  == '\r')) s.remove_suffix(1);
    return s;
}

bool next_token(std::string_view& s, std::string_view& tok, char delim = ' ') noexcept {
    while (!s.empty() && s.front() == delim) s.remove_prefix(1);
    if (s.empty()) return false;
    auto pos = s.find(delim);
    if (pos == std::string_view::npos) {
        tok = s;
        s   = {};
    } else {
        tok = s.substr(0, pos);
        s   = s.substr(pos + 1);
    }
    return true;
}

bool parse_uint(std::string_view s, uint32_t& out) noexcept {
    out = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{};
}

bool parse_uint8(std::string_view s, uint8_t& out) noexcept {
    uint32_t tmp = 0;
    if (!parse_uint(s, tmp)) return false;
    if (tmp > 0xFF) return false;
    out = static_cast<uint8_t>(tmp);
    return true;
}

SdpMedia::Kind kind_from_string(std::string_view s) noexcept {
    if (s == "video")        return SdpMedia::Kind::video;
    if (s == "audio")        return SdpMedia::Kind::audio;
    if (s == "application")  return SdpMedia::Kind::application;
    return SdpMedia::Kind::unknown;
}

void parse_fmtp_kv(std::string_view raw, std::unordered_map<std::string, std::string>& out) noexcept {
    // raw 形如 "profile-level-id=42e01e;packetization-mode=1;sprop-parameter-sets=Z01..."
    while (!raw.empty()) {
        auto sep = raw.find(';');
        std::string_view kv = (sep == std::string_view::npos) ? raw : raw.substr(0, sep);
        kv = trim(kv);
        if (!kv.empty()) {
            auto eq = kv.find('=');
            if (eq != std::string_view::npos) {
                std::string k{trim(kv.substr(0, eq))};
                std::string v{trim(kv.substr(eq + 1))};
                std::transform(k.begin(), k.end(), k.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                out.emplace(std::move(k), std::move(v));
            }
        }
        if (sep == std::string_view::npos) break;
        raw.remove_prefix(sep + 1);
    }
}

SdpFmtp* find_or_make_fmtp(SdpMedia& m, uint8_t pt) noexcept {
    for (auto& f : m.fmtp) {
        if (f.payload_type == pt) return &f;
    }
    m.fmtp.push_back(SdpFmtp{});
    m.fmtp.back().payload_type = pt;
    return &m.fmtp.back();
}

void apply_attribute(SdpSession& sess, SdpMedia* m, std::string_view value) noexcept {
    auto colon = value.find(':');
    std::string_view key  = (colon == std::string_view::npos) ? value : value.substr(0, colon);
    std::string_view body = (colon == std::string_view::npos) ? std::string_view{} : value.substr(colon + 1);
    key  = trim(key);
    body = trim(body);

    if (key == "rtpmap" && m) {
        // <pt> <name>/<clock>[/<channels>]
        std::string_view rest = body;
        std::string_view pt_str;
        if (!next_token(rest, pt_str, ' ')) return;
        SdpRtpMap rm;
        if (!parse_uint8(pt_str, rm.payload_type)) return;
        std::string_view enc;
        next_token(rest, enc, '/');
        rm.codec_name.assign(enc);
        std::transform(rm.codec_name.begin(), rm.codec_name.end(), rm.codec_name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        std::string_view clk;
        next_token(rest, clk, '/');
        uint32_t clk_v = 0;
        if (parse_uint(clk, clk_v)) rm.clock_rate_hz = clk_v;
        if (!rest.empty()) {
            uint8_t ch = 0;
            if (parse_uint8(rest, ch)) rm.channels = ch;
        }
        m->rtpmap.push_back(std::move(rm));
    } else if (key == "fmtp" && m) {
        std::string_view rest = body;
        std::string_view pt_str;
        if (!next_token(rest, pt_str, ' ')) return;
        uint8_t pt = 0;
        if (!parse_uint8(pt_str, pt)) return;
        SdpFmtp* f = find_or_make_fmtp(*m, pt);
        f->raw.assign(rest);
        parse_fmtp_kv(rest, f->params);
    } else if (key == "control") {
        if (m) m->control_uri.assign(body);
        else   sess.session_control_uri.assign(body);
    } else if (key == "rtcp-fb" && m) {
        // a=rtcp-fb:<pt|*> <type> [params]
        std::string_view rest = body;
        std::string_view pt_str, type;
        if (next_token(rest, pt_str, ' ') && next_token(rest, type, ' ')) {
            if (type == "nack") {
                m->has_rtcp_fb_nack = true;
                if (!rest.empty() && trim(rest) == "pli") {
                    m->has_rtcp_fb_pli = true;
                }
            } else if (type == "pli") {
                m->has_rtcp_fb_pli = true;
            } else if (type == "trr-int") {
                uint32_t ms = 0;
                if (parse_uint(trim(rest), ms)) m->trr_int_ms = static_cast<int>(ms);
            }
        }
    }
}

}  // namespace

std::optional<SdpSession> SdpParser::parse(std::string_view text) noexcept {
    SdpSession sess;
    SdpMedia*  current = nullptr;

    while (!text.empty()) {
        auto eol = text.find('\n');
        std::string_view line = (eol == std::string_view::npos) ? text : text.substr(0, eol);
        line = trim(line);
        if (eol == std::string_view::npos) text = {};
        else text = text.substr(eol + 1);
        if (line.size() < 2 || line[1] != '=') continue;

        const char ch = line[0];
        std::string_view value = line.substr(2);

        switch (ch) {
            case 's':
                sess.session_name.assign(value);
                break;
            case 'c': {
                // c=IN IP4 host
                std::string_view rest = value, t;
                next_token(rest, t, ' ');                          // nettype
                next_token(rest, t, ' ');                          // addrtype
                if (!rest.empty()) {
                    auto slash = rest.find('/');
                    std::string_view addr = (slash == std::string_view::npos) ? rest : rest.substr(0, slash);
                    if (current) current->connection_addr.assign(addr);
                    else         sess.default_connection_addr.assign(addr);
                }
                break;
            }
            case 'm': {
                // m=<kind> <port> <transport> <fmt>...
                std::string_view rest = value, kind_s, port_s, transport_s;
                if (!next_token(rest, kind_s, ' ')) break;
                if (!next_token(rest, port_s, ' ')) break;
                if (!next_token(rest, transport_s, ' ')) break;
                SdpMedia mm;
                mm.kind = kind_from_string(kind_s);
                uint32_t pv = 0;
                parse_uint(port_s, pv);
                mm.port = static_cast<uint16_t>(pv & 0xFFFF);
                mm.transport.assign(transport_s);
                while (!rest.empty()) {
                    std::string_view fmt;
                    if (!next_token(rest, fmt, ' ')) break;
                    uint8_t pt = 0;
                    if (parse_uint8(fmt, pt)) mm.payload_types.push_back(pt);
                }
                sess.media.push_back(std::move(mm));
                current = &sess.media.back();
                break;
            }
            case 'a':
                apply_attribute(sess, current, value);
                break;
            default:
                break;
        }
    }

    return sess;
}

std::optional<H264ProfileLevelId> parse_h264_profile_level_id(std::string_view hex) noexcept {
    if (hex.size() != 6) return std::nullopt;
    auto h = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int b[6];
    for (int i = 0; i < 6; ++i) { b[i] = h(hex[i]); if (b[i] < 0) return std::nullopt; }
    H264ProfileLevelId out;
    out.profile_idc      = static_cast<uint8_t>((b[0] << 4) | b[1]);
    out.constraint_flags = static_cast<uint8_t>((b[2] << 4) | b[3]);
    out.level_idc        = static_cast<uint8_t>((b[4] << 4) | b[5]);
    return out;
}

bool h264_profile_excludes_b_frames(const H264ProfileLevelId& plid) noexcept {
    // ITU-T H.264 Annex A profile_idc：
    //   66 = Baseline；constraint_set1=1 → Constrained Baseline（必无 B 帧）。
    //   77 = Main；100 = High（可含 B 帧），constraint_set4=1 + constraint_set5=1 → Constrained High（无 B 帧）。
    // 其它 profile（Extended 88 / High10 110 等）按规范可含 B，保守返 false。
    const uint8_t cs = plid.constraint_flags;
    const bool cs1 = (cs & 0x40) != 0;     // constraint_set1_flag
    const bool cs4 = (cs & 0x08) != 0;     // constraint_set4_flag
    const bool cs5 = (cs & 0x04) != 0;     // constraint_set5_flag
    if (plid.profile_idc == 66) return true;                        // Baseline / Constrained Baseline 都无 B
    if (plid.profile_idc == 100 && cs4 && cs5) return true;          // Constrained High
    if (cs1) return true;                                            // 任何 profile + constraint_set1 → 排除 B 帧
    return false;
}

}  // namespace mcp::transport
