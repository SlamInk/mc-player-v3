#include "transport/rtcp.h"

#include <cstring>

namespace mcp::transport {

namespace {

constexpr uint8_t kRtcpVersion = 2;

// PT
constexpr uint8_t kPtSr   = 200;
constexpr uint8_t kPtRr   = 201;
constexpr uint8_t kPtSdes = 202;
constexpr uint8_t kPtBye  = 203;
constexpr uint8_t kPtApp  = 204;
constexpr uint8_t kPtRtpFb= 205;     // FMT 1 = NACK
constexpr uint8_t kPtPsFb = 206;     // FMT 1 = PLI, 4 = FIR

// FMT
constexpr uint8_t kFmtNack = 1;
constexpr uint8_t kFmtPli  = 1;

// SDES item
constexpr uint8_t kSdesCname = 1;

uint16_t read_be16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
uint32_t read_be32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) <<  8) |
            static_cast<uint32_t>(p[3]);
}

void write_be16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}
void write_be32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >>  8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}

void write_rtcp_common(uint8_t* p, uint8_t pt, uint8_t fmt_or_count,
                       uint16_t length_words) noexcept {
    p[0] = static_cast<uint8_t>((kRtcpVersion << 6) | (fmt_or_count & 0x1F));
    p[1] = pt;
    write_be16(p + 2, length_words);
}

}  // namespace

bool RtcpReader::parse(std::span<const uint8_t> bytes,
                       std::vector<RtcpSenderReport>& srs,
                       std::vector<RtcpReceiverReport>& rrs,
                       std::vector<RtcpFeedbackNack>& nacks,
                       std::vector<RtcpFeedbackPli>& plis) noexcept {
    std::size_t off = 0;
    while (off + 4 <= bytes.size()) {
        const uint8_t* p = bytes.data() + off;
        const uint8_t version = p[0] >> 6;
        if (version != kRtcpVersion) return false;

        const uint8_t  rc_or_fmt = p[0] & 0x1F;
        const uint8_t  pt        = p[1];
        const uint16_t length_words = read_be16(p + 2);
        const std::size_t pkt_bytes = (static_cast<std::size_t>(length_words) + 1u) * 4u;
        if (off + pkt_bytes > bytes.size()) return false;

        if (pt == kPtSr) {
            if (pkt_bytes < 28) return false;
            RtcpSenderReport sr;
            sr.ssrc          = read_be32(p + 4);
            sr.ntp_timestamp = (static_cast<uint64_t>(read_be32(p + 8)) << 32) | read_be32(p + 12);
            sr.rtp_timestamp = read_be32(p + 16);
            sr.packet_count  = read_be32(p + 20);
            sr.octet_count   = read_be32(p + 24);
            srs.push_back(sr);
        } else if (pt == kPtRr) {
            if (pkt_bytes < 8) return false;
            RtcpReceiverReport rr;
            rr.ssrc = read_be32(p + 4);
            const uint8_t count = rc_or_fmt;
            std::size_t boff = 8;
            for (uint8_t i = 0; i < count; ++i) {
                if (boff + 24 > pkt_bytes) return false;
                RtcpReceiverReportBlock blk;
                blk.ssrc                  = read_be32(p + boff + 0);
                const uint32_t loss_word  = read_be32(p + boff + 4);
                blk.fraction_lost         = static_cast<uint8_t>((loss_word >> 24) & 0xFF);
                int32_t cum               = static_cast<int32_t>(loss_word & 0x00FFFFFF);
                if (cum & 0x00800000) cum |= 0xFF000000;  // sign extend
                blk.cumulative_lost       = cum;
                blk.extended_highest_seq  = read_be32(p + boff + 8);
                blk.interarrival_jitter   = read_be32(p + boff + 12);
                blk.last_sr               = read_be32(p + boff + 16);
                blk.delay_since_last_sr   = read_be32(p + boff + 20);
                rr.blocks.push_back(blk);
                boff += 24;
            }
            rrs.push_back(std::move(rr));
        } else if (pt == kPtRtpFb && rc_or_fmt == kFmtNack) {
            if (pkt_bytes < 12) return false;
            RtcpFeedbackNack nack;
            nack.sender_ssrc = read_be32(p + 4);
            nack.media_ssrc  = read_be32(p + 8);
            std::size_t fci = 12;
            while (fci + 4 <= pkt_bytes) {
                RtcpNackEntry e;
                e.pid = read_be16(p + fci);
                e.blp = read_be16(p + fci + 2);
                nack.entries.push_back(e);
                fci += 4;
            }
            nacks.push_back(std::move(nack));
        } else if (pt == kPtPsFb && rc_or_fmt == kFmtPli) {
            if (pkt_bytes < 12) return false;
            RtcpFeedbackPli pli;
            pli.sender_ssrc = read_be32(p + 4);
            pli.media_ssrc  = read_be32(p + 8);
            plis.push_back(pli);
        }
        // SDES / BYE / APP 等忽略；NACK/PLI 的接收方仅监听必需的反馈。

        off += pkt_bytes;
    }
    return off == bytes.size();
}

std::size_t RtcpWriter::write_receiver_report(uint32_t sender_ssrc,
                                               std::span<const RtcpReceiverReportBlock> blocks,
                                               std::span<uint8_t> out) noexcept {
    if (blocks.size() > 31) return 0;
    const std::size_t total = 8 + blocks.size() * 24;
    if (out.size() < total) return 0;
    uint8_t* p = out.data();
    write_rtcp_common(p, kPtRr, static_cast<uint8_t>(blocks.size()),
                      static_cast<uint16_t>(total / 4u - 1u));
    write_be32(p + 4, sender_ssrc);
    std::size_t off = 8;
    for (const auto& b : blocks) {
        write_be32(p + off + 0, b.ssrc);
        const uint32_t loss_word = (static_cast<uint32_t>(b.fraction_lost) << 24) |
                                   (static_cast<uint32_t>(b.cumulative_lost) & 0x00FFFFFF);
        write_be32(p + off + 4, loss_word);
        write_be32(p + off + 8,  b.extended_highest_seq);
        write_be32(p + off + 12, b.interarrival_jitter);
        write_be32(p + off + 16, b.last_sr);
        write_be32(p + off + 20, b.delay_since_last_sr);
        off += 24;
    }
    return total;
}

std::size_t RtcpWriter::write_sdes_cname(uint32_t sender_ssrc,
                                          std::span<const char> cname,
                                          std::span<uint8_t> out) noexcept {
    if (cname.size() > 255) return 0;
    // header(4) + ssrc(4) + sdes item(2 + cname) → 4 字节对齐 padding
    const std::size_t item_bytes = 2 + cname.size();
    const std::size_t total_unaligned = 4 + 4 + item_bytes + 1; /* end marker = 0 */
    const std::size_t total = (total_unaligned + 3u) & ~static_cast<std::size_t>(3u);
    if (out.size() < total) return 0;

    uint8_t* p = out.data();
    write_rtcp_common(p, kPtSdes, 1, static_cast<uint16_t>(total / 4u - 1u));
    write_be32(p + 4, sender_ssrc);
    p[8] = kSdesCname;
    p[9] = static_cast<uint8_t>(cname.size());
    std::memcpy(p + 10, cname.data(), cname.size());

    // pad with zeros after end marker
    for (std::size_t i = 10 + cname.size(); i < total; ++i) {
        p[i] = 0;
    }
    return total;
}

std::size_t RtcpWriter::write_nack(uint32_t sender_ssrc, uint32_t media_ssrc,
                                    std::span<const RtcpNackEntry> entries,
                                    std::span<uint8_t> out) noexcept {
    if (entries.empty()) return 0;
    const std::size_t total = 12 + entries.size() * 4;
    if (out.size() < total) return 0;
    uint8_t* p = out.data();
    write_rtcp_common(p, kPtRtpFb, kFmtNack, static_cast<uint16_t>(total / 4u - 1u));
    write_be32(p + 4, sender_ssrc);
    write_be32(p + 8, media_ssrc);
    std::size_t off = 12;
    for (const auto& e : entries) {
        write_be16(p + off + 0, e.pid);
        write_be16(p + off + 2, e.blp);
        off += 4;
    }
    return total;
}

std::size_t RtcpWriter::write_pli(uint32_t sender_ssrc, uint32_t media_ssrc,
                                   std::span<uint8_t> out) noexcept {
    constexpr std::size_t kPliBytes = 12;
    if (out.size() < kPliBytes) return 0;
    uint8_t* p = out.data();
    write_rtcp_common(p, kPtPsFb, kFmtPli, static_cast<uint16_t>(kPliBytes / 4u - 1u));
    write_be32(p + 4, sender_ssrc);
    write_be32(p + 8, media_ssrc);
    return kPliBytes;
}

}  // namespace mcp::transport
