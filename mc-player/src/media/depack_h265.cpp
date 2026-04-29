#include "media/depack_h265.h"

#include <algorithm>
#include <array>
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

// ──────────────────────────────────────────────────────────
// 最小 H.265 SPS 解析：仅抽 sps_max_num_reorder_pics 与 VUI 颜色 4 字段。
// ──────────────────────────────────────────────────────────
struct BitReader {
    const uint8_t* p   = nullptr;
    std::size_t    n   = 0;
    std::size_t    pos = 0;
    bool           bad = false;
    uint32_t read_bits(int k) noexcept {
        uint32_t v = 0;
        for (int i = 0; i < k; ++i) {
            if (pos >= n * 8) { bad = true; return 0; }
            v = (v << 1) | static_cast<uint32_t>((p[pos >> 3] >> (7 - (pos & 7))) & 1u);
            ++pos;
        }
        return v;
    }
    bool     read_bit1() noexcept { return read_bits(1) != 0; }
    void     skip_bits(int k) noexcept { (void)read_bits(k); }
    uint32_t read_ue() noexcept {
        int zeros = 0;
        while (!bad && read_bits(1) == 0) {
            ++zeros;
            if (zeros > 32) { bad = true; return 0; }
        }
        if (bad) return 0;
        if (zeros == 0) return 0;
        const uint32_t body = read_bits(zeros);
        return (1u << zeros) - 1u + body;
    }
    int32_t read_se() noexcept {
        const uint32_t k = read_ue();
        return (k & 1u) ? static_cast<int32_t>((k + 1u) >> 1) : -static_cast<int32_t>(k >> 1);
    }
};

std::vector<uint8_t> rbsp_unescape(std::span<const uint8_t> ebsp) noexcept {
    std::vector<uint8_t> out;
    out.reserve(ebsp.size());
    int zeros = 0;
    for (uint8_t b : ebsp) {
        if (zeros >= 2 && b == 0x03) { zeros = 0; continue; }
        out.push_back(b);
        zeros = (b == 0) ? zeros + 1 : 0;
    }
    return out;
}

void skip_profile_tier_level(BitReader& br, int max_sub_layers_minus1) noexcept {
    // profile_tier_level() — 主层 11 字节 + 子层标志位（参考 codec_dxva_video.cpp 已有 parse_ptl 实现，
    // 此处做最小裁剪，跳过所有字段不读语义）。
    br.skip_bits(2 + 1 + 5);                    // profile_space + tier_flag + profile_idc
    br.skip_bits(32);                           // profile_compatibility_flag[32]
    br.skip_bits(48);                           // progressive/interlaced/non_packed/frame_only/compatibility_indications
    br.skip_bits(8);                            // general_level_idc
    if (max_sub_layers_minus1 > 0) {
        std::array<bool, 8> profile_present_flag{}, level_present_flag{};
        for (int i = 0; i < max_sub_layers_minus1; ++i) {
            profile_present_flag[i] = br.read_bit1();
            level_present_flag[i]   = br.read_bit1();
        }
        // reserved_zero_2bits aligned to 8
        for (int i = max_sub_layers_minus1; i < 8; ++i) br.skip_bits(2);
        for (int i = 0; i < max_sub_layers_minus1; ++i) {
            if (profile_present_flag[i]) br.skip_bits(2 + 1 + 5 + 32 + 48);
            if (level_present_flag[i])   br.skip_bits(8);
        }
    }
}

void parse_h265_sps(std::span<const uint8_t> sps_nal, H265SpsInfo& out) noexcept {
    out = H265SpsInfo{};
    if (sps_nal.size() < 5) return;
    auto rbsp = rbsp_unescape(sps_nal.subspan(2));     // 跳过 2B NAL header
    if (rbsp.size() < 4) return;
    BitReader br{rbsp.data(), rbsp.size(), 0, false};

    br.skip_bits(4);                                   // sps_video_parameter_set_id
    const uint32_t max_sub_layers_minus1 = br.read_bits(3);
    br.skip_bits(1);                                   // sps_temporal_id_nesting_flag
    skip_profile_tier_level(br, static_cast<int>(max_sub_layers_minus1));
    if (br.bad) return;
    (void)br.read_ue();                                // sps_seq_parameter_set_id
    const uint32_t chroma_format_idc = br.read_ue();
    if (chroma_format_idc == 3) br.skip_bits(1);
    out.pic_width  = br.read_ue();
    out.pic_height = br.read_ue();
    if (br.read_bit1()) {                              // conformance_window_flag
        (void)br.read_ue(); (void)br.read_ue();
        (void)br.read_ue(); (void)br.read_ue();
    }
    (void)br.read_ue();                                // bit_depth_luma_minus8
    (void)br.read_ue();                                // bit_depth_chroma_minus8
    (void)br.read_ue();                                // log2_max_pic_order_cnt_lsb_minus4
    const bool sub_layer_ordering_info_present = br.read_bit1();
    const uint32_t start = sub_layer_ordering_info_present ? 0 : max_sub_layers_minus1;
    uint32_t max_reorder_top = 0;
    for (uint32_t i = start; i <= max_sub_layers_minus1; ++i) {
        (void)br.read_ue();                            // sps_max_dec_pic_buffering_minus1
        const uint32_t mr = br.read_ue();              // sps_max_num_reorder_pics
        (void)br.read_ue();                            // sps_max_latency_increase_plus1
        if (i == max_sub_layers_minus1) max_reorder_top = mr;
    }
    out.max_num_reorder_pics = max_reorder_top;
    if (br.bad) return;
    out.parsed = true;     // reorder 已可信；VUI 颜色 v1 不解（RPS 变长需完整逻辑）
    (void)br.read_ue();                                // log2_min_luma_coding_block_size_minus3
    (void)br.read_ue();                                // log2_diff_max_min_luma_coding_block_size
    (void)br.read_ue();                                // log2_min_luma_transform_block_size_minus2
    (void)br.read_ue();                                // log2_diff_max_min_luma_transform_block_size
    (void)br.read_ue();                                // max_transform_hierarchy_depth_inter
    (void)br.read_ue();                                // max_transform_hierarchy_depth_intra
    // VUI 与 scaling_list / RPS 之后，v1 不解析。reorder 与启发式色彩兜底已足够。
}

}  // namespace

DepackH265::DepackH265(EmitFn emit) noexcept : emit_{std::move(emit)} {}

void DepackH265::set_sprop_vps(std::string_view base64) noexcept { vps_ = base64_decode(base64); }
void DepackH265::set_sprop_sps(std::string_view base64) noexcept {
    sps_ = base64_decode(base64);
    parse_sps_locked();
}
void DepackH265::set_sprop_pps(std::string_view base64) noexcept { pps_ = base64_decode(base64); }

void DepackH265::parse_sps_locked() noexcept {
    if (sps_.empty()) { sps_info_ = H265SpsInfo{}; return; }
    parse_h265_sps(std::span<const uint8_t>(sps_), sps_info_);
}

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
            else if (nt == 33) { sps_ = std::vector<uint8_t>(nal.begin(), nal.end()); parse_sps_locked(); }
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
                else if (nt == 33) { sps_ = std::vector<uint8_t>(nal.begin(), nal.end()); parse_sps_locked(); }
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
        else if (t == 33) { sps_ = std::vector<uint8_t>(payload.begin(), payload.end()); parse_sps_locked(); }
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
