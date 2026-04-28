#include "media/depack_h265.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "pal/log.h"

namespace mcp::media {

namespace {

const uint8_t kAnnexBStart[] = {0x00, 0x00, 0x00, 0x01};

void append_annexb(std::vector<uint8_t>& out, std::span<const uint8_t> nal) noexcept {
    out.insert(out.end(), std::begin(kAnnexBStart), std::end(kAnnexBStart));
    out.insert(out.end(), nal.begin(), nal.end());
}

// 取 NAL Type — 6 位，位于第一字节 bit1..bit6。
uint8_t h265_nal_type(uint8_t b0) noexcept { return static_cast<uint8_t>((b0 >> 1) & 0x3F); }

// 取 LayerId（6 位）：byte0 bit0 + byte1 bit1..bit5（高 5 位）。
uint8_t h265_layer_id(uint8_t b0, uint8_t b1) noexcept {
    return static_cast<uint8_t>(((b0 & 0x01) << 5) | ((b1 >> 3) & 0x1F));
}

// 取 TID（3 位）：byte1 bit5..bit7（低 3 位）。RFC 7798 / H.265 规定 nuh_temporal_id_plus1，
// TID = nuh_temporal_id_plus1 - 1，下游不强制减 1，重组时直接保留原低 3 位。
uint8_t h265_tid_plus1(uint8_t b1) noexcept { return static_cast<uint8_t>(b1 & 0x07); }

bool is_irap(uint8_t nal_type) noexcept {
    // IRAP 范围：BLA_W_LP(16) ~ RSV_IRAP_VCL23(23)。常用：IDR_W_RADL(19) / IDR_N_LP(20) / CRA_NUT(21)。
    return nal_type >= 16 && nal_type <= 23;
}

// 简化 base64 解码（与 DepackH264 同口径）。SPROP-VPS / -SPS / -PPS 都是 base64。
int base64_value(unsigned char c) noexcept {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::vector<uint8_t> base64_decode(std::string_view in) noexcept {
    std::vector<uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    uint32_t accum = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=' || c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
        const int v = base64_value(static_cast<unsigned char>(c));
        if (v < 0) continue;
        accum = (accum << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((accum >> bits) & 0xFF));
        }
    }
    return out;
}

constexpr uint8_t kNalApPacket   = 48;     // RFC 7798 §4.4.2 Aggregation Packet
constexpr uint8_t kNalFuPacket   = 49;     // RFC 7798 §4.4.3 Fragmentation Unit
constexpr uint8_t kNalPaciPacket = 50;     // 不支持

}  // namespace

DepackH265::DepackH265(EmitFn emit) noexcept : emit_{std::move(emit)} {}

void DepackH265::set_sprop_vps(std::string_view base64) noexcept { vps_ = base64_decode(base64); }
void DepackH265::set_sprop_sps(std::string_view base64) noexcept { sps_ = base64_decode(base64); }
void DepackH265::set_sprop_pps(std::string_view base64) noexcept { pps_ = base64_decode(base64); }

void DepackH265::on_rtp(int64_t pts_us, bool marker, std::span<const uint8_t> payload) noexcept {
    if (payload.size() < 2) return;

    // PTS 跳变 → 切前一个 AU。
    if (!au_buffer_.empty() && pts_us != current_pts_us_) {
        emit_au(current_pts_us_, /*with_extradata=*/saw_irap_in_au_);
    }
    current_pts_us_ = pts_us;

    const uint8_t b0 = payload[0];
    const uint8_t b1 = payload[1];
    const uint8_t t  = h265_nal_type(b0);

    if (t == kNalApPacket) {
        // Aggregation Packet：去掉 2B AP header，逐段 (2B size + NAL)。RFC 7798 §4.4.2。
        std::size_t i = 2;
        while (i + 2 <= payload.size()) {
            const uint16_t nal_size = static_cast<uint16_t>(
                (static_cast<uint16_t>(payload[i]) << 8) | payload[i + 1]);
            i += 2;
            if (i + nal_size > payload.size() || nal_size < 2) break;
            std::span<const uint8_t> nal = payload.subspan(i, nal_size);
            const uint8_t nt = h265_nal_type(nal[0]);
            append_annexb(au_buffer_, nal);
            if (is_irap(nt))     saw_irap_in_au_     = true;
            if (nt == 32)        vps_ = std::vector<uint8_t>(nal.begin(), nal.end());
            else if (nt == 33)   sps_ = std::vector<uint8_t>(nal.begin(), nal.end());
            else if (nt == 34)   pps_ = std::vector<uint8_t>(nal.begin(), nal.end());
            i += nal_size;
        }
    } else if (t == kNalFuPacket) {
        // FU Packet：1B FU indicator(payload[0]) + 1B FU header + payload。
        // 重组：复制 payload[0..1] 作为 NAL header 占位，将 type 域改为 FU header 内的 type。
        if (payload.size() < 3) return;
        const uint8_t fu_hdr = payload[2];
        const bool   start_bit = (fu_hdr & 0x80) != 0;
        const bool   end_bit   = (fu_hdr & 0x40) != 0;
        const uint8_t inner_t  = static_cast<uint8_t>(fu_hdr & 0x3F);

        if (start_bit) {
            fu_buffer_.clear();
            // 重建 2B NAL header：type 替换为 inner_t；保留 forbidden_zero_bit / LayerId / TID。
            const uint8_t new_b0 = static_cast<uint8_t>((b0 & 0x81) | ((inner_t & 0x3F) << 1));
            fu_buffer_.push_back(new_b0);
            fu_buffer_.push_back(b1);
            fu_layer_id_       = h265_layer_id(b0, b1);
            fu_tid_            = h265_tid_plus1(b1);
            fu_in_progress_    = true;
        }
        if (fu_in_progress_) {
            fu_buffer_.insert(fu_buffer_.end(), payload.begin() + 3, payload.end());
            if (end_bit) {
                std::span<const uint8_t> nal{fu_buffer_};
                const uint8_t nt = h265_nal_type(nal[0]);
                append_annexb(au_buffer_, nal);
                if (is_irap(nt))   saw_irap_in_au_ = true;
                if (nt == 32)      vps_ = std::vector<uint8_t>(nal.begin(), nal.end());
                else if (nt == 33) sps_ = std::vector<uint8_t>(nal.begin(), nal.end());
                else if (nt == 34) pps_ = std::vector<uint8_t>(nal.begin(), nal.end());
                fu_in_progress_ = false;
                fu_buffer_.clear();
            }
        }
    } else if (t == kNalPaciPacket) {
        MCP_LOGF(pal::LogLevel::warn, "DepackH265: PACI(50) not supported, dropping");
        refs_lost_ = true;
    } else if (t < 48) {
        // 单 NAL（含 VPS/SPS/PPS/Slice/SEI 等）。
        append_annexb(au_buffer_, payload);
        if (is_irap(t)) saw_irap_in_au_ = true;
        if (t == 32)      vps_ = std::vector<uint8_t>(payload.begin(), payload.end());
        else if (t == 33) sps_ = std::vector<uint8_t>(payload.begin(), payload.end());
        else if (t == 34) pps_ = std::vector<uint8_t>(payload.begin(), payload.end());
    } else {
        MCP_LOGF(pal::LogLevel::warn,
                 "DepackH265: NAL type %u in unspecified range, dropping", t);
        refs_lost_ = true;
    }

    if (marker) emit_au(pts_us, /*with_extradata=*/saw_irap_in_au_);
}

void DepackH265::emit_au(int64_t pts_us, bool with_extradata) noexcept {
    if (au_buffer_.empty()) return;

    H265AccessUnit au;
    au.pts_us = pts_us;
    if (with_extradata && !vps_.empty() && !sps_.empty() && !pps_.empty()) {
        au.annexb_bytes.reserve(au_buffer_.size() + vps_.size() + sps_.size() + pps_.size() + 12);
        append_annexb(au.annexb_bytes, vps_);
        append_annexb(au.annexb_bytes, sps_);
        append_annexb(au.annexb_bytes, pps_);
        au.annexb_bytes.insert(au.annexb_bytes.end(), au_buffer_.begin(), au_buffer_.end());
    } else {
        au.annexb_bytes = std::move(au_buffer_);
    }
    au.has_irap         = saw_irap_in_au_;
    au.has_recovery_sei = saw_recovery_in_au_;
    au.refs_lost        = refs_lost_;
    au.params_present   = !vps_.empty() && !sps_.empty() && !pps_.empty();

    if (au.has_irap || au.has_recovery_sei) refs_lost_ = false;

    if (emit_) emit_(std::move(au));

    au_buffer_.clear();
    saw_irap_in_au_     = false;
    saw_recovery_in_au_ = false;
}

void DepackH265::mark_reference_lost() noexcept { refs_lost_ = true; }

void DepackH265::reset() noexcept {
    au_buffer_.clear();
    fu_buffer_.clear();
    fu_in_progress_     = false;
    saw_irap_in_au_     = false;
    saw_recovery_in_au_ = false;
    refs_lost_          = true;
    current_pts_us_     = 0;
}

}  // namespace mcp::media
