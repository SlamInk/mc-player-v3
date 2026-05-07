#include "media/dxva_h264.h"

#include <algorithm>
#include <cstring>

namespace mcp::media::h264 {

namespace {

// ─── Bit reader（与 codec_dxva_video.cpp HEVC 路径相同 RBSP 比特读取）────
class BitReader {
public:
    BitReader(const uint8_t* d, std::size_t n) noexcept : data_{d}, bits_{n * 8} {}

    uint32_t read_bits(uint32_t n) noexcept {
        if (n == 0 || pos_ + n > bits_) { bad_ = pos_ + n > bits_; return 0; }
        uint32_t v = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const std::size_t byte = pos_ >> 3;
            const uint32_t    bit  = 7u - static_cast<uint32_t>(pos_ & 7);
            v = (v << 1) | ((data_[byte] >> bit) & 1u);
            ++pos_;
        }
        return v;
    }
    bool     read_bit1() noexcept { return read_bits(1) != 0; }

    // ue(v) — Exp-Golomb 无符号
    uint32_t read_ue() noexcept {
        uint32_t lz = 0;
        while (lz < 32 && pos_ < bits_) {
            if (read_bits(1) == 1) break;
            ++lz;
        }
        if (lz >= 32 || bad_) { bad_ = true; return 0; }
        const uint32_t suf = lz == 0 ? 0u : read_bits(lz);
        return (1u << lz) - 1u + suf;
    }
    // se(v) — Exp-Golomb 有符号
    int32_t read_se() noexcept {
        const uint32_t k = read_ue();
        return (k & 1u) ? static_cast<int32_t>((k + 1) >> 1)
                        : -static_cast<int32_t>(k >> 1);
    }
    void skip_bits(uint32_t n) noexcept {
        if (pos_ + n > bits_) { bad_ = true; pos_ = bits_; return; }
        pos_ += n;
    }
    [[nodiscard]] bool bad() const noexcept { return bad_; }

    // H.264 §7.2 more_rbsp_data():
    //   RBSP trailing bits = 1 个 stop_one_bit + 0..7 个 alignment_zero_bit (到字节边界)。
    //   "More RBSP data" 当且仅当当前 bit 位置之后,还存在比 trailing 更靠后的 '1' bit。
    //
    // 实现:从 RBSP 末尾向前扫描,定位最后一个 '1' bit (即 stop_one_bit) 的 bit 索引;
    //       若 pos_ 严格小于该索引 → 还有真实语法 → true。
    [[nodiscard]] bool more_rbsp_data() const noexcept {
        if (pos_ >= bits_) return false;
        std::size_t last_set = 0;
        bool        found    = false;
        const std::size_t total_bytes = (bits_ + 7) / 8;
        for (std::size_t b = total_bytes; b > 0 && !found; --b) {
            const std::size_t byte_idx = b - 1;
            const uint8_t     byte     = data_[byte_idx];
            if (byte == 0) continue;
            for (int bit = 0; bit < 8; ++bit) {
                if ((byte >> bit) & 1u) {
                    last_set = byte_idx * 8u + (7u - static_cast<std::size_t>(bit));
                    found    = true;
                    break;
                }
            }
        }
        if (!found) return false;
        return pos_ < last_set;
    }

private:
    const uint8_t* data_;
    std::size_t    bits_;
    std::size_t    pos_{0};
    bool           bad_{false};
};

// scaling_list 跳读（H.264 §7.3.2.1.1.1）— 仅推进比特位置不存储。
void skip_scaling_list(BitReader& br, int size) noexcept {
    int last_scale = 8, next_scale = 8;
    for (int j = 0; j < size; ++j) {
        if (next_scale != 0) {
            const int32_t delta = br.read_se();
            next_scale = (last_scale + delta + 256) & 0xFF;
        }
        last_scale = (next_scale == 0) ? last_scale : next_scale;
    }
}

// 跳读 SPS scaling matrix（仅 chroma_format_idc=1 → 8 个 4x4 + 0 个 8x8 base，
// transform_8x8 时再额外读 2 个 8x8）。
void skip_seq_scaling_matrix(BitReader& br, uint8_t chroma_format_idc,
                              uint8_t fallback_8x8_count) noexcept {
    const int n_4x4 = (chroma_format_idc != 3) ? 8 : 12;
    for (int i = 0; i < n_4x4; ++i) {
        if (br.read_bit1()) skip_scaling_list(br, 16);
    }
    for (int i = 0; i < fallback_8x8_count; ++i) {
        if (br.read_bit1()) skip_scaling_list(br, 64);
    }
}

// hrd_parameters（VUI 内）— 仅推进比特，不存储。
void skip_hrd_parameters(BitReader& br) noexcept {
    const uint32_t cpb_cnt_minus1 = br.read_ue();
    br.skip_bits(4 + 4);                                  // bit_rate_scale + cpb_size_scale
    for (uint32_t i = 0; i <= cpb_cnt_minus1; ++i) {
        (void)br.read_ue();                                // bit_rate_value_minus1
        (void)br.read_ue();                                // cpb_size_value_minus1
        br.skip_bits(1);                                   // cbr_flag
    }
    br.skip_bits(5 + 5 + 5 + 5);                           // initial_cpb_removal/cpb_removal/dpb_output/time_offset
}

void parse_vui(BitReader& br, Sps& s) noexcept {
    if (br.read_bit1()) {                                  // aspect_ratio_info_present_flag
        const uint8_t idc = static_cast<uint8_t>(br.read_bits(8));
        if (idc == 255) br.skip_bits(16 + 16);
    }
    if (br.read_bit1()) br.skip_bits(1);                   // overscan_info_present + appropriate
    if (br.read_bit1()) {                                  // video_signal_type_present_flag
        br.skip_bits(3);                                   // video_format
        s.video_full_range_flag           = static_cast<uint8_t>(br.read_bits(1));
        s.colour_description_present_flag = static_cast<uint8_t>(br.read_bits(1));
        if (s.colour_description_present_flag) {
            s.colour_primaries         = static_cast<uint8_t>(br.read_bits(8));
            s.transfer_characteristics = static_cast<uint8_t>(br.read_bits(8));
            s.matrix_coefficients      = static_cast<uint8_t>(br.read_bits(8));
        }
    }
    if (br.read_bit1()) { (void)br.read_ue(); (void)br.read_ue(); }   // chroma_loc_info
    if (br.read_bit1()) br.skip_bits(32 + 32 + 1);                     // timing_info: num_units + time_scale + fixed_frame_rate

    const bool nal_hrd_present = br.read_bit1();
    if (nal_hrd_present) skip_hrd_parameters(br);
    const bool vcl_hrd_present = br.read_bit1();
    if (vcl_hrd_present) skip_hrd_parameters(br);
    if (nal_hrd_present || vcl_hrd_present) br.skip_bits(1);            // low_delay_hrd_flag
    br.skip_bits(1);                                                     // pic_struct_present_flag

    s.bitstream_restriction_flag = static_cast<uint8_t>(br.read_bits(1));
    if (s.bitstream_restriction_flag) {
        br.skip_bits(1);                                   // motion_vectors_over_pic_boundaries
        (void)br.read_ue();                                 // max_bytes_per_pic_denom
        (void)br.read_ue();                                 // max_bits_per_mb_denom
        (void)br.read_ue();                                 // log2_max_mv_length_horizontal
        (void)br.read_ue();                                 // log2_max_mv_length_vertical
        s.max_num_reorder_frames = br.read_ue();
        (void)br.read_ue();                                 // max_dec_frame_buffering
    }
}

}  // namespace (anonymous)

bool parse_sps(std::span<const uint8_t> rbsp, Sps& out) noexcept {
    if (rbsp.size() < 4) return false;
    BitReader br{rbsp.data(), rbsp.size()};
    out.profile_idc          = static_cast<uint8_t>(br.read_bits(8));
    out.constraint_set_flags = static_cast<uint8_t>(br.read_bits(6));
    br.skip_bits(2);                                       // reserved_zero_2bits
    out.level_idc            = static_cast<uint8_t>(br.read_bits(8));
    out.id                   = static_cast<uint8_t>(br.read_ue());

    // FRExt profile 系列才有 chroma_format_idc 等扩展字段
    const bool is_frext =
        out.profile_idc == 100 || out.profile_idc == 110 ||
        out.profile_idc == 122 || out.profile_idc == 244 ||
        out.profile_idc == 44  || out.profile_idc == 83  ||
        out.profile_idc == 86  || out.profile_idc == 118 ||
        out.profile_idc == 128 || out.profile_idc == 138 ||
        out.profile_idc == 139 || out.profile_idc == 134 ||
        out.profile_idc == 135;

    uint8_t fallback_8x8 = 0;
    if (is_frext) {
        out.chroma_format_idc = static_cast<uint8_t>(br.read_ue());
        if (out.chroma_format_idc == 3) {
            out.separate_colour_plane_flag = static_cast<uint8_t>(br.read_bits(1));
        }
        out.bit_depth_luma_minus8                 = static_cast<uint8_t>(br.read_ue());
        out.bit_depth_chroma_minus8               = static_cast<uint8_t>(br.read_ue());
        out.qpprime_y_zero_transform_bypass_flag  = static_cast<uint8_t>(br.read_bits(1));
        const bool seq_scaling_matrix_present = br.read_bit1();
        if (seq_scaling_matrix_present) {
            // v1 不支持自定义 scaling matrix —— 推进比特但拒绝该 SPS。
            skip_seq_scaling_matrix(br, out.chroma_format_idc, fallback_8x8);
            return false;
        }
    }

    // v1 实装边界：必须 4:2:0 + 8-bit，否则 DXVA decoder config 不会接受。
    if (out.chroma_format_idc != 1) return false;
    if (out.bit_depth_luma_minus8 != 0 || out.bit_depth_chroma_minus8 != 0) return false;

    out.log2_max_frame_num_minus4 = br.read_ue();
    out.pic_order_cnt_type        = static_cast<uint8_t>(br.read_ue());
    if (out.pic_order_cnt_type == 0) {
        out.log2_max_pic_order_cnt_lsb_minus4 = br.read_ue();
    } else if (out.pic_order_cnt_type == 1) {
        // type=1 IPC 流不出现，且 POC 推导依赖偏移表 — v1 拒绝。
        return false;
    } else if (out.pic_order_cnt_type != 2) {
        return false;
    }

    out.num_ref_frames                       = static_cast<uint8_t>(br.read_ue());
    out.gaps_in_frame_num_value_allowed_flag = static_cast<uint8_t>(br.read_bits(1));
    out.pic_width_in_mbs_minus1              = br.read_ue();
    out.pic_height_in_map_units_minus1       = br.read_ue();
    out.frame_mbs_only_flag                  = static_cast<uint8_t>(br.read_bits(1));
    if (!out.frame_mbs_only_flag) {
        // v1 不处理 MBAFF / interlace。
        return false;
    }
    out.direct_8x8_inference_flag = static_cast<uint8_t>(br.read_bits(1));
    out.frame_cropping_flag       = static_cast<uint8_t>(br.read_bits(1));
    if (out.frame_cropping_flag) {
        out.frame_crop_left_offset   = br.read_ue();
        out.frame_crop_right_offset  = br.read_ue();
        out.frame_crop_top_offset    = br.read_ue();
        out.frame_crop_bottom_offset = br.read_ue();
    }
    out.vui_parameters_present_flag = static_cast<uint8_t>(br.read_bits(1));
    if (out.vui_parameters_present_flag) {
        parse_vui(br, out);
    }

    if (br.bad()) return false;
    out.pic_width_in_samples_l  = (out.pic_width_in_mbs_minus1 + 1u) * 16u;
    out.pic_height_in_samples_l = (out.pic_height_in_map_units_minus1 + 1u) * 16u
                                  * (2u - out.frame_mbs_only_flag);
    out.valid = true;
    return true;
}

bool parse_pps(std::span<const uint8_t> rbsp, const Sps& sps, Pps& out) noexcept {
    BitReader br{rbsp.data(), rbsp.size()};
    out.id     = static_cast<uint8_t>(br.read_ue());
    out.sps_id = static_cast<uint8_t>(br.read_ue());
    if (out.sps_id != sps.id) return false;

    out.entropy_coding_mode_flag = static_cast<uint8_t>(br.read_bits(1));
    out.bottom_field_pic_order_in_frame_present_flag = static_cast<uint8_t>(br.read_bits(1));
    out.num_slice_groups_minus1                       = static_cast<uint8_t>(br.read_ue());
    if (out.num_slice_groups_minus1 != 0) {
        // v1 不支持 FMO。
        return false;
    }
    out.num_ref_idx_l0_default_active_minus1 = static_cast<uint8_t>(br.read_ue());
    out.num_ref_idx_l1_default_active_minus1 = static_cast<uint8_t>(br.read_ue());
    out.weighted_pred_flag        = static_cast<uint8_t>(br.read_bits(1));
    out.weighted_bipred_idc       = static_cast<uint8_t>(br.read_bits(2));
    out.pic_init_qp_minus26       = static_cast<int8_t>(br.read_se());
    out.pic_init_qs_minus26       = static_cast<int8_t>(br.read_se());
    out.chroma_qp_index_offset    = static_cast<int8_t>(br.read_se());
    out.deblocking_filter_control_present_flag = static_cast<uint8_t>(br.read_bits(1));
    out.constrained_intra_pred_flag            = static_cast<uint8_t>(br.read_bits(1));
    out.redundant_pic_cnt_present_flag         = static_cast<uint8_t>(br.read_bits(1));

    // H.264 §7.3.2.2：FRExt 扩展字段不按 profile 门控,而按 more_rbsp_data() 判别。
    //   if (more_rbsp_data()) {
    //       transform_8x8_mode_flag      u(1)
    //       pic_scaling_matrix_present   u(1)
    //       if (pic_scaling_matrix_present) for(...) scaling_list
    //       second_chroma_qp_index_offset se(v)
    //   }
    // 历史 bug：用 sps.profile_idc∈FRExt 列表替代 more_rbsp_data() —— 当 High profile 编码器精简地
    //   只发到 redundant_pic_cnt_present_flag 即写 stop_bit + padding 时,旧代码会把 0x80 的 stop_bit
    //   误读成 transform_8x8_mode_flag=1,后续比特位移错位 → DXVA PicParams 字段污染 → driver silent fail
    //   → DPB 全 0 → NV12 渲染呈 BT.709 limited 暗绿。
    if (br.more_rbsp_data()) {
        out.transform_8x8_mode_flag         = static_cast<uint8_t>(br.read_bits(1));
        out.pic_scaling_matrix_present_flag = static_cast<uint8_t>(br.read_bits(1));
        if (out.pic_scaling_matrix_present_flag) {
            // v1 不支持自定义 scaling matrix。
            return false;
        }
        out.second_chroma_qp_index_offset = static_cast<int8_t>(br.read_se());
    } else {
        out.second_chroma_qp_index_offset = out.chroma_qp_index_offset;
    }

    if (br.bad()) return false;
    out.valid = true;
    return true;
}

bool parse_slice_header_min(std::span<const uint8_t> rbsp,
                              uint8_t                  nal_type,
                              const Sps&               sps,
                              const Pps&               pps,
                              SliceHdr&                out) noexcept {
    BitReader br{rbsp.data(), rbsp.size()};
    out.first_mb_in_slice = br.read_ue();
    out.slice_type        = br.read_ue() % 5;          // 0/5,1/6,...,4/9 → mod 5 取基本类型
    out.pps_id            = static_cast<uint8_t>(br.read_ue());
    if (out.pps_id != pps.id) return false;

    if (sps.separate_colour_plane_flag) {
        out.colour_plane_id = static_cast<uint8_t>(br.read_bits(2));
    }
    out.frame_num = br.read_bits(sps.log2_max_frame_num_minus4 + 4);

    if (!sps.frame_mbs_only_flag) {
        out.field_pic_flag = static_cast<uint8_t>(br.read_bits(1));
        if (out.field_pic_flag) {
            out.bottom_field_flag = static_cast<uint8_t>(br.read_bits(1));
        }
    }
    if (is_idr(nal_type)) {
        out.idr_pic_id = br.read_ue();
    }
    if (sps.pic_order_cnt_type == 0) {
        out.pic_order_cnt_lsb = br.read_bits(sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
        if (pps.bottom_field_pic_order_in_frame_present_flag && !out.field_pic_flag) {
            out.delta_pic_order_cnt_bottom = br.read_se();
        }
    }
    // type=1 的 delta_pic_order_cnt 已在 parse_sps 中拒绝。
    // type=2 不读 POC 字段，POC 由 frame_num 推导。

    if (br.bad()) return false;
    out.valid = true;
    return true;
}

int32_t compute_poc(PocState&      state,
                    const Sps&     sps,
                    const SliceHdr& sh,
                    uint8_t         nal_type,
                    uint8_t         nal_ref_idc) noexcept {
    if (sps.pic_order_cnt_type == 0) {
        const int32_t max_lsb = 1 << (sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
        int32_t prev_msb = state.prev_pic_order_cnt_msb;
        int32_t prev_lsb = state.prev_pic_order_cnt_lsb;
        if (is_idr(nal_type)) { prev_msb = 0; prev_lsb = 0; }

        const int32_t lsb = static_cast<int32_t>(sh.pic_order_cnt_lsb);
        int32_t msb = 0;
        if (lsb < prev_lsb && (prev_lsb - lsb) >= max_lsb / 2) {
            msb = prev_msb + max_lsb;
        } else if (lsb > prev_lsb && (lsb - prev_lsb) > max_lsb / 2) {
            msb = prev_msb - max_lsb;
        } else {
            msb = prev_msb;
        }
        const int32_t poc = msb + lsb;
        if (nal_ref_idc != 0) {
            state.prev_pic_order_cnt_msb = msb;
            state.prev_pic_order_cnt_lsb = lsb;
        }
        return poc;
    }

    if (sps.pic_order_cnt_type == 2) {
        // §8.2.1.3：FrameNumOffset 在 frame_num 回绕时 +max_frame_num；IDR 时归零。
        const uint32_t max_frame_num = 1u << (sps.log2_max_frame_num_minus4 + 4);
        uint32_t frame_num_offset = 0;
        if (is_idr(nal_type)) {
            frame_num_offset = 0;
        } else if (state.prev_frame_num > sh.frame_num) {
            frame_num_offset = state.prev_frame_num_offset + max_frame_num;
        } else {
            frame_num_offset = state.prev_frame_num_offset;
        }
        const int32_t frame_num_total = static_cast<int32_t>(frame_num_offset + sh.frame_num);
        const int32_t poc = (nal_ref_idc != 0) ? (2 * frame_num_total)
                                                 : (2 * frame_num_total - 1);

        state.prev_frame_num        = sh.frame_num;
        state.prev_frame_num_offset = frame_num_offset;
        return poc;
    }

    return 0;     // type=1 已拒绝；防御性 return
}

namespace {

// DXVA_PicEntry_H264 编码 — SDK 头明确:
//   CurrPic       — AssociatedFlag = bottom_field_flag (frame-coded 进 0)
//   RefFrameList  — AssociatedFlag = long_term flag    (短时 ref 进 0)
// bPicEntry == 0xFF 表示 unused ref slot;非 0xFF 时 Index7Bits=DPB 索引。
DXVA_PicEntry_H264 make_curr_pic(uint8_t dpb_idx, uint8_t bottom_field) noexcept {
    DXVA_PicEntry_H264 e{};
    if (dpb_idx == 0xFF) {
        e.bPicEntry = 0xFF;
    } else {
        e.Index7Bits     = static_cast<UCHAR>(dpb_idx & 0x7Fu);
        e.AssociatedFlag = static_cast<UCHAR>(bottom_field & 0x01u);
    }
    return e;
}

DXVA_PicEntry_H264 make_ref_pic(uint8_t dpb_idx, uint8_t long_term_flag) noexcept {
    DXVA_PicEntry_H264 e{};
    if (dpb_idx == 0xFF) {
        e.bPicEntry = 0xFF;
    } else {
        e.Index7Bits     = static_cast<UCHAR>(dpb_idx & 0x7Fu);
        e.AssociatedFlag = static_cast<UCHAR>(long_term_flag & 0x01u);
    }
    return e;
}

}  // namespace

void fill_pic_params(DXVA_PicParams_H264&         pp,
                      const Sps&                   sps,
                      const Pps&                   pps,
                      const SliceHdr&              sh,
                      uint8_t                      nal_type,
                      uint8_t                      nal_ref_idc,
                      uint32_t                     curr_dpb_idx,
                      int32_t                      curr_poc,
                      uint32_t                     curr_frame_num,
                      std::span<const RefPic>      refs,
                      uint16_t                     status_report_id) noexcept {
    std::memset(&pp, 0, sizeof(pp));

    pp.wFrameWidthInMbsMinus1  = static_cast<USHORT>(sps.pic_width_in_mbs_minus1);
    pp.wFrameHeightInMbsMinus1 = static_cast<USHORT>(sps.pic_height_in_map_units_minus1);

    // CurrPic — progressive 路径,SDK 注释 "/* flag is bot field flag */"。
    pp.CurrPic = make_curr_pic(static_cast<uint8_t>(curr_dpb_idx),
                                sh.bottom_field_flag);
    pp.CurrFieldOrderCnt[0] = curr_poc;
    pp.CurrFieldOrderCnt[1] = curr_poc;

    pp.num_ref_frames = sps.num_ref_frames;

    // wBitFields（H.264 §spec 字段位）
    pp.field_pic_flag                          = sh.field_pic_flag;
    pp.MbaffFrameFlag                          = (sps.mb_adaptive_frame_field_flag && !sh.field_pic_flag) ? 1 : 0;
    pp.residual_colour_transform_flag          = sps.separate_colour_plane_flag;
    pp.sp_for_switch_flag                      = 0;
    pp.chroma_format_idc                       = sps.chroma_format_idc;
    pp.RefPicFlag                              = (nal_ref_idc != 0) ? 1 : 0;
    pp.constrained_intra_pred_flag             = pps.constrained_intra_pred_flag;
    pp.weighted_pred_flag                      = pps.weighted_pred_flag;
    pp.weighted_bipred_idc                     = pps.weighted_bipred_idc;
    pp.MbsConsecutiveFlag                      = 1;       // 不支持 FMO，恒 1
    pp.frame_mbs_only_flag                     = sps.frame_mbs_only_flag;
    pp.transform_8x8_mode_flag                 = pps.transform_8x8_mode_flag;
    pp.MinLumaBipredSize8x8Flag                = (sps.level_idc >= 31) ? 1 : 0;
    pp.IntraPicFlag                            = (sh.slice_type == 2 || sh.slice_type == 4 || is_idr(nal_type)) ? 1 : 0;

    pp.bit_depth_luma_minus8   = sps.bit_depth_luma_minus8;
    pp.bit_depth_chroma_minus8 = sps.bit_depth_chroma_minus8;

    // DXVA H.264 spec quirk: Reserved16Bits 实际不是 reserved,是 driver 私有 scaling
    // list mode 标志。ffmpeg dxva2_h264.c::ff_dxva2_h264_fill_picture_parameters 默认
    // 设 3 (FIXME comment: "is there a way to detect the right mode?"),Intel ClearVideo
    // 老 driver 走 0x34c workaround,部分 driver 接 0。Intel UHD 730 实测设 0 让 driver
    // 静默 reject SubmitDecoderBuffers (深绿屏:emit 但 NV12 全 0)。设 3 对齐 ffmpeg
    // 默认。本字段在文件后部仍被 reset 为 0 — 这里先设是为了保护 fill_pic_params 调用
    // 顺序;后续 zero 是 H.264 spec original 定义。
    pp.Reserved16Bits          = 3;

    pp.StatusReportFeedbackNumber = status_report_id ? status_report_id : 1;

    // RefFrameList[16] 默认全 0xFF (未占用),SDK 注释 "/* flag LT */" — AssociatedFlag = long_term。
    for (auto& e : pp.RefFrameList) e.bPicEntry = 0xFF;
    for (auto& fc : pp.FieldOrderCntList) { fc[0] = 0; fc[1] = 0; }
    for (auto& fn : pp.FrameNumList) fn = 0;
    pp.UsedForReferenceFlags = 0;
    pp.NonExistingFrameFlags = 0;

    int slot = 0;
    for (const auto& r : refs) {
        if (slot >= 16) break;
        if (!r.used || r.dpb_index == 0xFF) continue;
        pp.RefFrameList[slot]              = make_ref_pic(r.dpb_index, r.long_term);
        pp.FieldOrderCntList[slot][0]      = r.field_order_cnt[0];
        pp.FieldOrderCntList[slot][1]      = r.field_order_cnt[1];
        pp.FrameNumList[slot]              = static_cast<USHORT>(r.frame_num);
        // UsedForReferenceFlags:每张 ref 两 bit (top + bottom);progressive 都置 1。
        pp.UsedForReferenceFlags |= (0x3u << (2 * slot));
        ++slot;
    }

    pp.frame_num                             = static_cast<USHORT>(curr_frame_num);
    pp.log2_max_frame_num_minus4             = static_cast<UCHAR>(sps.log2_max_frame_num_minus4);
    pp.pic_order_cnt_type                    = sps.pic_order_cnt_type;
    pp.log2_max_pic_order_cnt_lsb_minus4     = static_cast<UCHAR>(sps.log2_max_pic_order_cnt_lsb_minus4);
    pp.delta_pic_order_always_zero_flag      = sps.delta_pic_order_always_zero_flag;
    pp.direct_8x8_inference_flag             = sps.direct_8x8_inference_flag;
    pp.entropy_coding_mode_flag              = pps.entropy_coding_mode_flag;
    pp.pic_order_present_flag                = pps.bottom_field_pic_order_in_frame_present_flag;
    pp.num_slice_groups_minus1               = pps.num_slice_groups_minus1;
    pp.slice_group_map_type                  = 0;
    pp.deblocking_filter_control_present_flag = pps.deblocking_filter_control_present_flag;
    pp.redundant_pic_cnt_present_flag        = pps.redundant_pic_cnt_present_flag;
    pp.Reserved8BitsA                        = 0;
    pp.Reserved8BitsB                        = 0;
    pp.slice_group_change_rate_minus1        = 0;
    // SliceGroupMap 仅在 num_slice_groups_minus1 > 0 时使用,此处恒 0(已禁用 FMO)。

    pp.num_ref_idx_l0_active_minus1          = pps.num_ref_idx_l0_default_active_minus1;
    pp.num_ref_idx_l1_active_minus1          = pps.num_ref_idx_l1_default_active_minus1;
    pp.pic_init_qs_minus26                   = pps.pic_init_qs_minus26;
    pp.pic_init_qp_minus26                   = pps.pic_init_qp_minus26;
    pp.chroma_qp_index_offset                = pps.chroma_qp_index_offset;
    pp.second_chroma_qp_index_offset         = pps.second_chroma_qp_index_offset;

    pp.ContinuationFlag                       = 1;
    // 注:Reserved16Bits 已在前部设 3(driver scaling list mode 标志),不再 reset 为 0。
    // ContinuationFlag = 1 让 driver 按完整 PicParams 字段读;0 = 早期 mode 1 仅读
    // 前部小段(罕用)。
}

DXVA_Slice_H264_Short make_slice_short(uint32_t bs_offset, uint32_t bs_bytes) noexcept {
    DXVA_Slice_H264_Short s{};
    s.BSNALunitDataLocation = bs_offset;
    s.SliceBytesInBuffer    = bs_bytes;
    s.wBadSliceChopping     = 0;
    return s;
}

}  // namespace mcp::media::h264
