/*
 * RTP 包头解析（RFC 3550 §5.1） + 头扩展（RFC 8285）。
 *
 * 不做包构造，仅解析。RTP 数据来源：
 *   - RTSP UDP：socket recv 直接拿到完整 datagram。
 *   - RTSP TCP interleaved：去掉 4 字节 `$ ch len` 包头后即 RTP 包。
 *
 * 调用方持有原始 buffer；本结构存指针/长度切片，不拷贝。
 */

#ifndef MC_PLAYER_TRANSPORT_RTP_PACKET_H_
#define MC_PLAYER_TRANSPORT_RTP_PACKET_H_

#include <cstdint>
#include <span>

namespace mcp::transport {

struct RtpHeaderExtension {
    uint16_t                    profile = 0;        // RFC 8285 = 0xBEDE (one-byte) / 0x100x (two-byte)
    std::span<const uint8_t>    body;               // 不含 4 字节扩展 header
};

struct RtpPacket {
    uint8_t                     version       = 0;
    bool                        padding       = false;
    bool                        extension_flag= false;
    uint8_t                     csrc_count    = 0;
    bool                        marker        = false;
    uint8_t                     payload_type  = 0;
    uint16_t                    sequence      = 0;
    uint32_t                    timestamp     = 0;
    uint32_t                    ssrc          = 0;
    std::span<const uint32_t>   csrc_list;          // big-endian source ids
    RtpHeaderExtension          extension;
    std::span<const uint8_t>    payload;            // 已剔除 padding
};

/// 解析一个 datagram 为 RtpPacket。失败返回 false，*pkt 内容未定义。
bool parse_rtp(std::span<const uint8_t> datagram, RtpPacket* pkt) noexcept;

/// abs-send-time 头扩展（RFC 8285 + draft-alvestrand-rtcweb-abs-send-time）。
/// id = 1..14（one-byte）；body 长度 3 字节，编码为 24-bit fixed-point seconds Q18.6。
/// 失败（缺扩展或 id 不匹配）返回 false。
bool extract_abs_send_time(const RtpHeaderExtension& ext, uint8_t expected_id,
                           uint32_t* out_24bit) noexcept;

/// MID 头扩展（RFC 9143 BUNDLE 路由）。
bool extract_mid(const RtpHeaderExtension& ext, uint8_t expected_id,
                 std::span<const uint8_t>* out) noexcept;

}  // namespace mcp::transport

#endif  // MC_PLAYER_TRANSPORT_RTP_PACKET_H_
