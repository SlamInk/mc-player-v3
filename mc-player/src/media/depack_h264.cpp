#include "media/depack_h264.h"

#include <algorithm>
#include <cstring>

#include "pal/log.h"

namespace mcp::media {

namespace {

constexpr uint8_t kNalTypeSlice         = 1;
constexpr uint8_t kNalTypeIdr           = 5;
constexpr uint8_t kNalTypeSei           = 6;
constexpr uint8_t kNalTypeSps           = 7;
constexpr uint8_t kNalTypePps           = 8;
constexpr uint8_t kNalTypeStapA         = 24;
constexpr uint8_t kNalTypeFuA           = 28;

// SEI payload type 6 = recovery_point。
constexpr uint8_t kSeiRecoveryPoint     = 6;

const uint8_t kAnnexBStart[] = {0x00, 0x00, 0x00, 0x01};

void append_annexb(std::vector<uint8_t>& out, std::span<const uint8_t> nal) noexcept {
    out.insert(out.end(), std::begin(kAnnexBStart), std::end(kAnnexBStart));
    out.insert(out.end(), nal.begin(), nal.end());
}

uint8_t nal_type(uint8_t header_byte) noexcept {
    return static_cast<uint8_t>(header_byte & 0x1F);
}

// 简化 base64 解码——SDP sprop-parameter-sets 永远 ASCII 子集（A-Za-z0-9+/=）。
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

bool sei_has_recovery_point_zero(std::span<const uint8_t> nal_no_header) noexcept {
    // SEI payload sequence: each = (type, size, payload, more...). Loop until trailing 0x80 or end.
    // recovery_frame_cnt 是无符号 ue(v)；read first byte's leading zeros to estimate length.
    std::size_t i = 0;
    while (i < nal_no_header.size()) {
        uint32_t pt = 0;
        while (i < nal_no_header.size() && nal_no_header[i] == 0xFF) {
            pt += 0xFF;
            ++i;
        }
        if (i >= nal_no_header.size()) return false;
        pt += nal_no_header[i++];

        uint32_t sz = 0;
        while (i < nal_no_header.size() && nal_no_header[i] == 0xFF) {
            sz += 0xFF;
            ++i;
        }
        if (i >= nal_no_header.size()) return false;
        sz += nal_no_header[i++];

        if (pt == kSeiRecoveryPoint) {
            // recovery_frame_cnt = ue(v)；首位非 0 即代表 cnt==0 当首字节高 bit = 1。
            // 简化判定：首字节高 bit == 1 表示 ue(v)==0。
            if (sz >= 1 && i < nal_no_header.size()) {
                return (nal_no_header[i] & 0x80) != 0;
            }
            return false;
        }
        i += sz;
    }
    return false;
}

}  // namespace

DepackH264::DepackH264(EmitFn emit) noexcept : emit_{std::move(emit)} {}

void DepackH264::set_sprop_parameter_sets(std::string_view base64_csv) noexcept {
    sps_.clear();
    pps_.clear();
    while (!base64_csv.empty()) {
        auto comma = base64_csv.find(',');
        std::string_view item = (comma == std::string_view::npos)
                                ? base64_csv : base64_csv.substr(0, comma);
        auto raw = base64_decode(item);
        if (!raw.empty()) {
            const uint8_t t = nal_type(raw.front());
            if (t == kNalTypeSps && sps_.empty()) {
                sps_ = std::move(raw);
            } else if (t == kNalTypePps && pps_.empty()) {
                pps_ = std::move(raw);
            }
        }
        if (comma == std::string_view::npos) break;
        base64_csv.remove_prefix(comma + 1);
    }
}

void DepackH264::on_rtp(int64_t pts_us, bool marker, std::span<const uint8_t> payload) noexcept {
    if (payload.empty()) return;

    // 帧边界识别：PTS 跳变 → 切前一个 AU 出去。
    if (!au_buffer_.empty() && pts_us != current_pts_us_) {
        emit_au(current_pts_us_, /*with_extradata=*/false);
    }
    current_pts_us_ = pts_us;

    const uint8_t hdr = payload[0];
    const uint8_t t   = nal_type(hdr);

    if (t >= kNalTypeSlice && t <= 23) {
        // Single NAL
        append_annexb(au_buffer_, payload);
        if (t == kNalTypeIdr) saw_idr_in_au_ = true;
        if (t == kNalTypeSps) sps_.assign(payload.begin(), payload.end());
        if (t == kNalTypePps) pps_.assign(payload.begin(), payload.end());
        if (t == kNalTypeSei && payload.size() > 1 &&
            sei_has_recovery_point_zero(payload.subspan(1))) {
            saw_recovery_in_au_ = true;
        }
    } else if (t == kNalTypeStapA) {
        // STAP-A: 1B header + N×(2B size + NAL)
        std::size_t i = 1;
        while (i + 2 <= payload.size()) {
            const uint16_t nal_size = static_cast<uint16_t>(
                (static_cast<uint16_t>(payload[i]) << 8) | payload[i + 1]);
            i += 2;
            if (i + nal_size > payload.size()) break;
            std::span<const uint8_t> nal = payload.subspan(i, nal_size);
            append_annexb(au_buffer_, nal);
            const uint8_t nt = nal_type(nal.front());
            if (nt == kNalTypeIdr) saw_idr_in_au_ = true;
            if (nt == kNalTypeSps) sps_.assign(nal.begin(), nal.end());
            if (nt == kNalTypePps) pps_.assign(nal.begin(), nal.end());
            if (nt == kNalTypeSei && nal.size() > 1 &&
                sei_has_recovery_point_zero(nal.subspan(1))) {
                saw_recovery_in_au_ = true;
            }
            i += nal_size;
        }
    } else if (t == kNalTypeFuA) {
        // FU-A: 1B FU indicator + 1B FU header + payload
        if (payload.size() < 2) return;
        const uint8_t fu_ind = payload[0];
        const uint8_t fu_hdr = payload[1];
        const bool   start  = (fu_hdr & 0x80) != 0;
        const bool   end    = (fu_hdr & 0x40) != 0;
        const uint8_t in_t  = static_cast<uint8_t>(fu_hdr & 0x1F);

        if (start) {
            fu_buffer_.clear();
            fu_in_progress_     = true;
            fu_nal_header_byte_ = static_cast<uint8_t>((fu_ind & 0xE0) | in_t);
            fu_buffer_.push_back(fu_nal_header_byte_);
        }
        if (fu_in_progress_) {
            fu_buffer_.insert(fu_buffer_.end(), payload.begin() + 2, payload.end());
            if (end) {
                append_annexb(au_buffer_, std::span<const uint8_t>(fu_buffer_));
                if (in_t == kNalTypeIdr) saw_idr_in_au_ = true;
                if (in_t == kNalTypeSps) sps_.assign(fu_buffer_.begin(), fu_buffer_.end());
                if (in_t == kNalTypePps) pps_.assign(fu_buffer_.begin(), fu_buffer_.end());
                if (in_t == kNalTypeSei && fu_buffer_.size() > 1 &&
                    sei_has_recovery_point_zero(std::span<const uint8_t>(fu_buffer_).subspan(1))) {
                    saw_recovery_in_au_ = true;
                }
                fu_in_progress_ = false;
                fu_buffer_.clear();
            }
        }
    } else {
        // 25/26/27/29 等 packetization-mode=2 的封装在 v1 不接受；丢帧并标 invalid。
        MCP_LOGF(pal::LogLevel::warn,
                 "DepackH264: rejected NAL type %u (packetization-mode=2 not supported)", t);
        refs_lost_ = true;
    }

    if (marker) {
        emit_au(pts_us, /*with_extradata=*/saw_idr_in_au_);
    }
}

void DepackH264::emit_au(int64_t pts_us, bool with_extradata) noexcept {
    if (au_buffer_.empty()) return;

    H264AccessUnit au;
    au.pts_us = pts_us;
    if (with_extradata && !sps_.empty() && !pps_.empty()) {
        // 把 SPS/PPS 前置到 AU 头，给硬件解码器作 extradata。
        au.annexb_bytes.reserve(au_buffer_.size() + sps_.size() + pps_.size() + 8);
        append_annexb(au.annexb_bytes, sps_);
        append_annexb(au.annexb_bytes, pps_);
        au.annexb_bytes.insert(au.annexb_bytes.end(), au_buffer_.begin(), au_buffer_.end());
    } else {
        au.annexb_bytes = std::move(au_buffer_);
    }
    au.has_idr          = saw_idr_in_au_;
    au.has_recovery_sei = saw_recovery_in_au_;
    au.refs_lost        = refs_lost_;
    au.params_present   = !sps_.empty() && !pps_.empty();

    // refresh anchor → 解 invalid。
    if (au.has_idr || au.has_recovery_sei) {
        refs_lost_ = false;
    }

    if (emit_) emit_(std::move(au));

    au_buffer_.clear();
    saw_idr_in_au_      = false;
    saw_recovery_in_au_ = false;
}

void DepackH264::mark_reference_lost() noexcept {
    refs_lost_ = true;
}

void DepackH264::reset() noexcept {
    au_buffer_.clear();
    fu_buffer_.clear();
    fu_in_progress_     = false;
    saw_idr_in_au_      = false;
    saw_recovery_in_au_ = false;
    refs_lost_          = true;
    current_pts_us_     = 0;
    // 不清 SPS/PPS：可能由 SDP set_sprop_parameter_sets 注入，重连不应丢失。
}

}  // namespace mcp::media
