/*
 * SDP 解析（RFC 4566 / RFC 8866）— 仅提取 RTSP 路径所需字段。
 *
 * 关注的字段：
 *   m=<media> <port> <transport> <fmt>...    — 媒体行（video / audio）
 *   c=<nettype> <addrtype> <connection-address>
 *   a=rtpmap:<pt> <name>/<clock>[/<channels>]
 *   a=fmtp:<pt> <params>                       — 含 sprop-parameter-sets / config / profile-level-id
 *   a=control:<uri>                            — per-media control URI（RTSP SETUP target）
 *   a=range:<value>                            — 仅诊断
 *
 * 不构造 SDP，只解析。所有字符串视图指向调用方拥有的 buffer。
 */

#ifndef MC_PLAYER_TRANSPORT_SDP_PARSER_H_
#define MC_PLAYER_TRANSPORT_SDP_PARSER_H_

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mcp::transport {

struct SdpRtpMap {
    uint8_t     payload_type    = 0;
    std::string codec_name;             // "H264" / "H265" / "MPEG4-GENERIC" / "OPUS" / "PCMA"...
    uint32_t    clock_rate_hz   = 0;
    uint8_t     channels        = 1;    // 仅音频
};

struct SdpFmtp {
    uint8_t                     payload_type = 0;
    std::string                 raw;            // "profile-level-id=42e01e;packetization-mode=1;..."
    std::unordered_map<std::string, std::string> params;
};

struct SdpMedia {
    enum class Kind { unknown, video, audio, application };

    Kind                    kind             = Kind::unknown;
    uint16_t                port             = 0;
    std::string             transport;          // "RTP/AVP" / "RTP/AVPF" / "RTP/SAVP"
    std::vector<uint8_t>    payload_types;
    std::string             control_uri;        // a=control（相对或绝对）
    std::string             connection_addr;    // 可被 m= 之上 c= 行覆盖
    std::vector<SdpRtpMap>  rtpmap;
    std::vector<SdpFmtp>    fmtp;
    bool                    has_rtcp_fb_nack    = false;
    bool                    has_rtcp_fb_pli     = false;
    int                     trr_int_ms          = -1;       // -1 = 未指定
};

struct SdpSession {
    std::string             session_name;
    std::string             session_control_uri;
    std::string             default_connection_addr;
    std::vector<SdpMedia>   media;
};

class SdpParser {
public:
    /// 解析整段 SDP（CRLF 或 LF 行尾均可）。失败返回 std::nullopt。
    static std::optional<SdpSession> parse(std::string_view text) noexcept;
};

}  // namespace mcp::transport

#endif  // MC_PLAYER_TRANSPORT_SDP_PARSER_H_
