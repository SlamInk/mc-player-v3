#include "transport/rtp_packet.h"

#include <bit>

namespace mcp::transport {

namespace {

constexpr std::size_t kRtpFixedHeaderBytes = 12;
constexpr uint8_t     kRtpVersion          = 2;

constexpr uint16_t kExtProfileOneByte = 0xBEDE;
constexpr uint16_t kExtProfileTwoByteMask = 0xFFF0;
constexpr uint16_t kExtProfileTwoByte = 0x1000;

uint16_t read_be16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
uint32_t read_be32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) <<  8) |
            static_cast<uint32_t>(p[3]);
}

}  // namespace

bool parse_rtp(std::span<const uint8_t> datagram, RtpPacket* pkt) noexcept {
    if (!pkt || datagram.size() < kRtpFixedHeaderBytes) return false;
    const uint8_t* p = datagram.data();

    const uint8_t b0 = p[0];
    const uint8_t b1 = p[1];

    pkt->version        = static_cast<uint8_t>(b0 >> 6);
    pkt->padding        = (b0 & 0x20) != 0;
    pkt->extension_flag = (b0 & 0x10) != 0;
    pkt->csrc_count     = static_cast<uint8_t>(b0 & 0x0F);
    pkt->marker         = (b1 & 0x80) != 0;
    pkt->payload_type   = static_cast<uint8_t>(b1 & 0x7F);

    if (pkt->version != kRtpVersion) return false;

    pkt->sequence  = read_be16(p + 2);
    pkt->timestamp = read_be32(p + 4);
    pkt->ssrc      = read_be32(p + 8);

    std::size_t offset = kRtpFixedHeaderBytes;

    // CSRC 列表
    const std::size_t csrc_bytes = static_cast<std::size_t>(pkt->csrc_count) * sizeof(uint32_t);
    if (datagram.size() < offset + csrc_bytes) return false;
    pkt->csrc_list = std::span<const uint32_t>(
        reinterpret_cast<const uint32_t*>(p + offset), pkt->csrc_count);
    offset += csrc_bytes;

    // 头扩展 (RFC 8285)
    if (pkt->extension_flag) {
        if (datagram.size() < offset + 4) return false;
        const uint16_t profile = read_be16(p + offset);
        const uint16_t length_words = read_be16(p + offset + 2);
        offset += 4;

        const std::size_t ext_bytes = static_cast<std::size_t>(length_words) * 4u;
        if (datagram.size() < offset + ext_bytes) return false;

        pkt->extension.profile = profile;
        pkt->extension.body    = std::span<const uint8_t>(p + offset, ext_bytes);
        offset += ext_bytes;
    } else {
        pkt->extension = {};
    }

    // padding：尾部字节给出 padding 字节数
    std::size_t end = datagram.size();
    if (pkt->padding) {
        if (end == 0) return false;
        const uint8_t pad = datagram[end - 1];
        if (pad == 0 || pad > (end - offset)) return false;
        end -= pad;
    }

    if (end < offset) return false;
    pkt->payload = std::span<const uint8_t>(p + offset, end - offset);
    return true;
}

bool extract_abs_send_time(const RtpHeaderExtension& ext, uint8_t expected_id,
                           uint32_t* out_24bit) noexcept {
    if (!out_24bit || expected_id == 0 || expected_id > 14) return false;
    if (ext.profile != kExtProfileOneByte) return false;     // 仅 one-byte form 支持

    std::size_t i = 0;
    while (i < ext.body.size()) {
        const uint8_t hdr = ext.body[i++];
        if (hdr == 0) continue;                              // padding
        const uint8_t id  = hdr >> 4;
        const uint8_t len = static_cast<uint8_t>((hdr & 0x0F) + 1);
        if (i + len > ext.body.size()) return false;
        if (id == expected_id && len == 3) {
            const uint8_t* b = &ext.body[i];
            *out_24bit = (static_cast<uint32_t>(b[0]) << 16) |
                         (static_cast<uint32_t>(b[1]) <<  8) |
                          static_cast<uint32_t>(b[2]);
            return true;
        }
        i += len;
    }
    return false;
}

bool extract_mid(const RtpHeaderExtension& ext, uint8_t expected_id,
                 std::span<const uint8_t>* out) noexcept {
    if (!out || expected_id == 0 || expected_id > 14) return false;
    if (ext.profile != kExtProfileOneByte) return false;

    std::size_t i = 0;
    while (i < ext.body.size()) {
        const uint8_t hdr = ext.body[i++];
        if (hdr == 0) continue;
        const uint8_t id  = hdr >> 4;
        const uint8_t len = static_cast<uint8_t>((hdr & 0x0F) + 1);
        if (i + len > ext.body.size()) return false;
        if (id == expected_id) {
            *out = std::span<const uint8_t>(ext.body.subspan(i, len));
            return true;
        }
        i += len;
    }
    return false;
}

}  // namespace mcp::transport
