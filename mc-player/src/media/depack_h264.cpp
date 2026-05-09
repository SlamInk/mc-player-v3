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

// ──────────────────────────────────────────────────────────
// 最小 ExpGolomb / RBSP unescape，仅用于 SPS 解析到 VUI 段。
// 不做完整 SPS 语义校验——失败时设 parsed=false 不影响主路径。
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
            // H.264 §9.1 ue(v) 最大 32 leading zeros 对应 (1<<32)-1 上限;
            // zeros == 32 触发 (1u << 32) 是 UB(shift >= unsigned type 宽度),
            // 收紧为 zeros >= 32 set_bad,与 spec 上限对齐(允许 codeNum 表示 [0, 2^32-2])。
            if (zeros >= 32) { bad = true; return 0; }
        }
        if (bad) return 0;
        if (zeros == 0) return 0;
        uint32_t body = read_bits(zeros);
        return (1u << zeros) - 1u + body;
    }
    int32_t read_se() noexcept {
        uint32_t k = read_ue();
        return (k & 1u) ? static_cast<int32_t>((k + 1u) >> 1) : -static_cast<int32_t>(k >> 1);
    }
};

std::vector<uint8_t> rbsp_unescape(std::span<const uint8_t> ebsp) noexcept {
    // 删除 emulation prevention byte 0x03 in pattern 00 00 03 → 00 00。
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

bool profile_has_extended_chroma(uint8_t profile_idc) noexcept {
    switch (profile_idc) {
        case 100: case 110: case 122: case 244: case 44:
        case 83:  case 86:  case 118: case 128: case 138:
        case 139: case 134: case 135:
            return true;
        default: return false;
    }
}

void parse_h264_sps(std::span<const uint8_t> sps_nal, H264SpsInfo& out) noexcept {
    out = H264SpsInfo{};
    if (sps_nal.size() < 4) return;
    auto rbsp = rbsp_unescape(sps_nal.subspan(1));     // 跳过 1B NAL header
    if (rbsp.size() < 3) return;

    BitReader br{rbsp.data(), rbsp.size(), 0, false};
    out.profile_idc      = static_cast<uint8_t>(br.read_bits(8));
    out.constraint_flags = static_cast<uint8_t>(br.read_bits(8));
    out.level_idc        = static_cast<uint8_t>(br.read_bits(8));
    (void)br.read_ue();     // seq_parameter_set_id

    if (profile_has_extended_chroma(out.profile_idc)) {
        const uint32_t chroma_idc = br.read_ue();
        if (chroma_idc == 3) br.skip_bits(1);
        (void)br.read_ue();                            // bit_depth_luma_minus8
        (void)br.read_ue();                            // bit_depth_chroma_minus8
        br.skip_bits(1);                               // qpprime_y_zero_transform_bypass_flag
        const bool seq_scaling_matrix_present_flag = br.read_bit1();
        if (seq_scaling_matrix_present_flag) {
            const int n_lists = (chroma_idc == 3) ? 12 : 8;
            for (int i = 0; i < n_lists; ++i) {
                if (br.read_bit1()) {
                    // 跳过 scaling list（用 ue 序列驱动 delta_scale，最长 64）
                    const int sz = (i < 6) ? 16 : 64;
                    int last_scale = 8, next_scale = 8;
                    for (int j = 0; j < sz; ++j) {
                        if (next_scale != 0) {
                            const int32_t delta = br.read_se();
                            next_scale = (last_scale + delta + 256) % 256;
                        }
                        last_scale = (next_scale == 0) ? last_scale : next_scale;
                    }
                }
            }
        }
    }

    (void)br.read_ue();                                // log2_max_frame_num_minus4
    const uint32_t poc_type = br.read_ue();
    if (poc_type == 0) {
        (void)br.read_ue();                            // log2_max_pic_order_cnt_lsb_minus4
    } else if (poc_type == 1) {
        br.skip_bits(1);                               // delta_pic_order_always_zero_flag
        (void)br.read_se();                            // offset_for_non_ref_pic
        (void)br.read_se();                            // offset_for_top_to_bottom_field
        const uint32_t n = br.read_ue();
        if (n > 256) { return; }                       // 防御：异常值
        for (uint32_t i = 0; i < n; ++i) (void)br.read_se();
    }
    (void)br.read_ue();                                // max_num_ref_frames
    br.skip_bits(1);                                   // gaps_in_frame_num_value_allowed_flag
    out.pic_width_in_mbs        = br.read_ue() + 1u;
    out.pic_height_in_map_units = br.read_ue() + 1u;
    const bool frame_mbs_only_flag = br.read_bit1();
    if (!frame_mbs_only_flag) br.skip_bits(1);
    br.skip_bits(1);                                   // direct_8x8_inference_flag
    if (br.read_bit1()) {                              // frame_cropping_flag
        (void)br.read_ue(); (void)br.read_ue();
        (void)br.read_ue(); (void)br.read_ue();
    }

    if (br.bad) return;
    if (!br.read_bit1()) {                              // vui_parameters_present_flag
        out.parsed = true;
        return;
    }

    // VUI parameters
    if (br.read_bit1()) {                              // aspect_ratio_info_present_flag
        const uint32_t aspect_ratio_idc = br.read_bits(8);
        if (aspect_ratio_idc == 255) br.skip_bits(32); // sar_width 16 + sar_height 16
    }
    if (br.read_bit1()) br.skip_bits(1);               // overscan_info_present_flag → overscan_appropriate_flag
    if (br.read_bit1()) {                              // video_signal_type_present_flag
        br.skip_bits(3);                               // video_format
        out.video_full_range_flag = br.read_bit1();
        if (br.read_bit1()) {                          // colour_description_present_flag
            out.colour_primaries         = static_cast<int>(br.read_bits(8));
            out.transfer_characteristics = static_cast<int>(br.read_bits(8));
            out.matrix_coefficients      = static_cast<int>(br.read_bits(8));
        }
    }
    if (br.read_bit1()) {                              // chroma_loc_info_present_flag
        (void)br.read_ue();
        (void)br.read_ue();
    }
    if (br.read_bit1()) {                              // timing_info_present_flag
        br.skip_bits(32 + 32 + 1);
    }
    const bool nal_hrd = br.read_bit1();
    auto skip_hrd = [&] {
        const uint32_t cpb_cnt = br.read_ue() + 1u;
        if (cpb_cnt > 32) { br.bad = true; return; }
        br.skip_bits(4 + 4);                            // bit_rate_scale + cpb_size_scale
        for (uint32_t i = 0; i < cpb_cnt; ++i) {
            (void)br.read_ue(); (void)br.read_ue();
            br.skip_bits(1);
        }
        br.skip_bits(5 + 5 + 5 + 5);                    // 4×fixed-length 5-bit
    };
    if (nal_hrd) skip_hrd();
    const bool vcl_hrd = br.read_bit1();
    if (vcl_hrd) skip_hrd();
    if (nal_hrd || vcl_hrd) br.skip_bits(1);           // low_delay_hrd_flag
    br.skip_bits(1);                                   // pic_struct_present_flag

    if (br.read_bit1()) {                              // bitstream_restriction_flag
        out.bitstream_restriction = true;
        br.skip_bits(1);                               // motion_vectors_over_pic_boundaries_flag
        (void)br.read_ue();                            // max_bytes_per_pic_denom
        (void)br.read_ue();                            // max_bits_per_mb_denom
        (void)br.read_ue();                            // log2_max_mv_length_horizontal
        (void)br.read_ue();                            // log2_max_mv_length_vertical
        out.max_num_reorder_frames = br.read_ue();
        (void)br.read_ue();                            // max_dec_frame_buffering
    }
    out.parsed = !br.bad;
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
    parse_sps_locked();
}

void DepackH264::parse_sps_locked() noexcept {
    if (sps_.empty()) { sps_info_ = H264SpsInfo{}; return; }
    parse_h264_sps(std::span<const uint8_t>(sps_), sps_info_);
}

void DepackH264::on_rtp(int64_t pts_us, bool marker, std::span<const uint8_t> payload,
                         int64_t arrival_qpc_ns) noexcept {
    if (payload.empty()) return;

    // 帧边界识别：PTS 跳变 → 切前一个 AU 出去。
    if (!au_buffer_.empty() && pts_us != current_pts_us_) {
        emit_au(current_pts_us_, /*with_extradata=*/false);
    }
    // 端到端延时探针：取 AU 第一包的 arrival 戳（au_buffer_ 当前为空即 AU 起始）。
    if (au_buffer_.empty() && arrival_qpc_ns != 0) {
        current_arrival_qpc_ns_ = arrival_qpc_ns;
    }
    current_pts_us_ = pts_us;

    const uint8_t hdr = payload[0];
    const uint8_t t   = nal_type(hdr);

    if (t >= kNalTypeSlice && t <= 23) {
        // Single NAL
        append_annexb(au_buffer_, payload);
        if (t == kNalTypeIdr) saw_idr_in_au_ = true;
        if (t == kNalTypeSps) {
            sps_.assign(payload.begin(), payload.end());
            parse_sps_locked();
        }
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
            if (nt == kNalTypeSps) {
                sps_.assign(nal.begin(), nal.end());
                parse_sps_locked();
            }
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
            // 早期(start bit) 标识 IDR/SEI(recovery_point) — 之前只在 end bit 设置,
            // 中段丢包导致 end 永不到时 saw_idr_in_au_ 一直 false → emit_au
            // with_extradata=false → SPS/PPS 不前置 → codec 永远 init_decoder 不了
            // (rtsp://192.168.1.171/live/11 实测 root cause #2,丢包率高的 IPC 链路必触发)。
            // FU start bit 已含 in_t,IDR 标识在收到首个分片时即可决定。
            // 对齐 VLC modules/access/rtp/h264.c::rtp_h264_decode FU-A 路径:VLC 的 SDP
            // sprop xps 在 packetizer 第一个 NAL 前自动前置,不依赖 IDR 完整重组。
            if (in_t == kNalTypeIdr) saw_idr_in_au_ = true;
        }
        if (fu_in_progress_) {
            fu_buffer_.insert(fu_buffer_.end(), payload.begin() + 2, payload.end());
            if (end) {
                append_annexb(au_buffer_, std::span<const uint8_t>(fu_buffer_));
                // saw_idr_in_au_ 已在 start 时 set;此处保留 SPS/PPS/SEI 完整重组路径
                // (这些 NAL 必须完整才能 parse,FU 重组完整才提交)。
                if (in_t == kNalTypeSps) {
                    sps_.assign(fu_buffer_.begin(), fu_buffer_.end());
                    parse_sps_locked();
                }
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
    au.arrival_qpc_ns   = current_arrival_qpc_ns_;

    // refresh anchor → 解 invalid。
    if (au.has_idr || au.has_recovery_sei) {
        refs_lost_ = false;
    }

    if (emit_) emit_(std::move(au));

    au_buffer_.clear();
    saw_idr_in_au_      = false;
    saw_recovery_in_au_ = false;
    current_arrival_qpc_ns_ = 0;
}

void DepackH264::mark_reference_lost() noexcept {
    refs_lost_ = true;
    // RTP seq gap → 仅中断当前正在重组的 FU-A(丢的可能是 FU 中间分片,继续 push
    // 后续分片到 fu_buffer_ 会得损坏 NAL,driver 解出 partial 真实数据 + zero-fill
    // → 花屏)。其余状态保留,对齐 VLC modules/access/rtp/h264.c — VLC 在 seq gap
    // 不主动丢 au_buffer/saw_idr,只让 packetizer/decoder 自己拒不完整 slice。
    //
    // 历史误清(导致 root cause):
    //   旧代码同时清 au_buffer_ + saw_idr_in_au_。RTSP IPC 流的 IDR AU 通常 30KB+
    //   (~20 RTPs) 中段丢包概率极高,清 au_buffer 把已收到的 SPS/PPS NAL 全丢掉,
    //   清 saw_idr_in_au_ 让 emit_au with_extradata=false → 该 AU 不前置 SPS/PPS,
    //   下游 codec 永远拿不到 extradata 启动解码。每秒 5-10 个 gap=1 + IDR 期 5s
    //   一个 → 形成无限循环(rtsp://192.168.1.171/live/11 实测)。VLC 在同一流不会
    //   回退是因为它从 SDP sprop xps 直接前置不依赖带内 SPS 完整重组。
    //
    // 现行策略对齐 VLC:
    //   1. 不清 au_buffer_ — 已完整收到的 NAL(SPS/PPS/SEI/前 N 个 slice)留下;
    //      不完整 slice 由 codec 解码失败时被忽略,SPS/PPS 仍能被 parse_sps 看到。
    //   2. 不清 saw_idr_in_au_ — 已识别的 IDR 状态保留,emit_au 仍以 with_extradata=
    //      true 前置 SPS/PPS。
    //   3. 仅清 fu_buffer_ + fu_in_progress_ — in-flight FU 必须丢(append 后续 RTP
    //      会得损坏 NAL)。后续 RTP 见 FU start 重新开始,Single/STAP-A 直接进 au_buffer。
    //   4. refs_lost_ 仍 set — 让 codec 标记后续 frame.refs_armed=false,gate poison 配合。
    fu_buffer_.clear();
    fu_in_progress_ = false;
}

void DepackH264::reset() noexcept {
    au_buffer_.clear();
    fu_buffer_.clear();
    fu_in_progress_     = false;
    saw_idr_in_au_      = false;
    saw_recovery_in_au_ = false;
    refs_lost_          = true;
    current_pts_us_     = 0;
    current_arrival_qpc_ns_ = 0;
    // 不清 SPS/PPS：可能由 SDP set_sprop_parameter_sets 注入，重连不应丢失。
}

}  // namespace mcp::media
