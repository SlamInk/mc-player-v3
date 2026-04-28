#include "hevc_parser.h"

#include <algorithm>
#include <cstring>

#include "bitstream.h"

namespace mclc {

namespace {

// §7.3.3 profile_tier_level — 简化：跳过 sub_layer profile/level 数据，仅取 general_*。
void parse_profile_tier_level(BitReader& br,
                               bool       profile_present_flag,
                               int        max_num_sub_layers_minus1,
                               ProfileTierLevel& out) noexcept {
    if (profile_present_flag) {
        out.general_profile_space  = static_cast<uint8_t>(br.read_bits(2));
        out.general_tier_flag      = static_cast<uint8_t>(br.read_bits(1));
        out.general_profile_idc    = static_cast<uint8_t>(br.read_bits(5));
        out.general_profile_compatibility_flag = br.read_bits(32);
        // general constraint flags：48 bit。
        br.skip_bits(48);
    }
    out.general_level_idc = static_cast<uint8_t>(br.read_bits(8));

    // sub_layer_profile_present_flag[i] / sub_layer_level_present_flag[i]
    uint8_t sub_layer_profile_present[kMaxSubLayers]{};
    uint8_t sub_layer_level_present[kMaxSubLayers]{};
    for (int i = 0; i < max_num_sub_layers_minus1; ++i) {
        sub_layer_profile_present[i] = static_cast<uint8_t>(br.read_bits(1));
        sub_layer_level_present[i]   = static_cast<uint8_t>(br.read_bits(1));
    }
    if (max_num_sub_layers_minus1 > 0) {
        for (int i = max_num_sub_layers_minus1; i < 8; ++i) br.skip_bits(2);  // reserved
    }
    for (int i = 0; i < max_num_sub_layers_minus1; ++i) {
        if (sub_layer_profile_present[i]) br.skip_bits(2 + 1 + 5 + 32 + 48);
        if (sub_layer_level_present[i])   br.skip_bits(8);
    }
}

// §7.3.7 short_term_ref_pic_set
void parse_st_rps(BitReader&                       br,
                   uint32_t                         st_rps_idx,
                   const std::vector<ShortTermRefPicSet>& list,
                   ShortTermRefPicSet&              out) noexcept {
    bool inter_ref_pic_set_prediction_flag = false;
    if (st_rps_idx != 0) inter_ref_pic_set_prediction_flag = br.read_bit1();

    if (inter_ref_pic_set_prediction_flag) {
        // 仅 SPS 内 RPS 列表用：delta_idx_minus1 默认 0。
        uint32_t delta_idx_minus1 = 0;
        if (st_rps_idx == list.size()) {
            delta_idx_minus1 = br.read_ue();
        }
        const uint32_t ref_rps_idx = st_rps_idx - (delta_idx_minus1 + 1);
        const bool delta_rps_sign  = br.read_bit1();
        const uint32_t abs_delta_rps_minus1 = br.read_ue();
        const int32_t delta_rps = (delta_rps_sign ? -1 : 1) *
                                    static_cast<int32_t>(abs_delta_rps_minus1 + 1);

        const ShortTermRefPicSet& ref = list[ref_rps_idx];
        const int n_ref = ref.num_negative_pics + ref.num_positive_pics + 1;
        std::vector<uint8_t> used_by_curr(n_ref, 0);
        std::vector<uint8_t> use_delta(n_ref, 1);
        for (int j = 0; j < n_ref; ++j) {
            used_by_curr[j] = static_cast<uint8_t>(br.read_bits(1));
            if (!used_by_curr[j]) use_delta[j] = static_cast<uint8_t>(br.read_bits(1));
        }

        // 重建 delta_poc 列表（§7.4.8）。
        int i = 0;
        for (int j = ref.num_positive_pics - 1; j >= 0; --j) {
            const int32_t d = ref.delta_poc_s1[j] + delta_rps;
            if (d < 0 && use_delta[ref.num_negative_pics + j]) {
                out.delta_poc_s0[i] = d;
                out.used_by_curr_pic_s0[i] = used_by_curr[ref.num_negative_pics + j];
                ++i;
            }
        }
        if (delta_rps < 0 && use_delta[n_ref - 1]) {
            out.delta_poc_s0[i] = delta_rps;
            out.used_by_curr_pic_s0[i] = used_by_curr[n_ref - 1];
            ++i;
        }
        for (int j = 0; j < ref.num_negative_pics; ++j) {
            const int32_t d = ref.delta_poc_s0[j] + delta_rps;
            if (d < 0 && use_delta[j]) {
                out.delta_poc_s0[i] = d;
                out.used_by_curr_pic_s0[i] = used_by_curr[j];
                ++i;
            }
        }
        out.num_negative_pics = i;

        i = 0;
        for (int j = ref.num_negative_pics - 1; j >= 0; --j) {
            const int32_t d = ref.delta_poc_s0[j] + delta_rps;
            if (d > 0 && use_delta[j]) {
                out.delta_poc_s1[i] = d;
                out.used_by_curr_pic_s1[i] = used_by_curr[j];
                ++i;
            }
        }
        if (delta_rps > 0 && use_delta[n_ref - 1]) {
            out.delta_poc_s1[i] = delta_rps;
            out.used_by_curr_pic_s1[i] = used_by_curr[n_ref - 1];
            ++i;
        }
        for (int j = 0; j < ref.num_positive_pics; ++j) {
            const int32_t d = ref.delta_poc_s1[j] + delta_rps;
            if (d > 0 && use_delta[ref.num_negative_pics + j]) {
                out.delta_poc_s1[i] = d;
                out.used_by_curr_pic_s1[i] = used_by_curr[ref.num_negative_pics + j];
                ++i;
            }
        }
        out.num_positive_pics = i;
    } else {
        out.num_negative_pics = static_cast<int>(br.read_ue());
        out.num_positive_pics = static_cast<int>(br.read_ue());
        if (out.num_negative_pics > 16 || out.num_positive_pics > 16) return;
        int32_t prev = 0;
        for (int i = 0; i < out.num_negative_pics; ++i) {
            const uint32_t delta_poc_s0_minus1 = br.read_ue();
            prev -= static_cast<int32_t>(delta_poc_s0_minus1 + 1);
            out.delta_poc_s0[i] = prev;
            out.used_by_curr_pic_s0[i] = static_cast<uint8_t>(br.read_bits(1));
        }
        prev = 0;
        for (int i = 0; i < out.num_positive_pics; ++i) {
            const uint32_t delta_poc_s1_minus1 = br.read_ue();
            prev += static_cast<int32_t>(delta_poc_s1_minus1 + 1);
            out.delta_poc_s1[i] = prev;
            out.used_by_curr_pic_s1[i] = static_cast<uint8_t>(br.read_bits(1));
        }
    }
}

// §E.2.1 vui_parameters — 仅取颜色相关字段，其余跳过。
void parse_vui(BitReader& br, HevcSps& sps) noexcept {
    if (br.read_bit1()) br.skip_bits(8);  // aspect_ratio_idc
    if (br.read_bit1()) {                  // aspect_ratio_idc == 255 → SAR_*
        br.skip_bits(16);                  // sar_width
        br.skip_bits(16);                  // sar_height
    }
    if (br.read_bit1()) br.skip_bits(1);   // overscan_info_present / overscan_appropriate

    if (br.read_bit1()) {                  // video_signal_type_present_flag
        br.skip_bits(3);                   // video_format
        sps.video_full_range_flag = static_cast<uint8_t>(br.read_bits(1));
        sps.colour_description_present_flag = static_cast<uint8_t>(br.read_bits(1));
        if (sps.colour_description_present_flag) {
            sps.colour_primaries           = static_cast<uint8_t>(br.read_bits(8));
            sps.transfer_characteristics   = static_cast<uint8_t>(br.read_bits(8));
            sps.matrix_coeffs              = static_cast<uint8_t>(br.read_bits(8));
        }
    }
    // 其余 VUI 字段 v1 不用，停止解析（不会影响主流程，bitstream tail 容忍）。
}

}  // namespace

bool parse_vps(std::span<const uint8_t> rbsp, HevcVps& out) noexcept {
    BitReader br{rbsp.data(), rbsp.size()};

    out.vps_id                  = static_cast<uint8_t>(br.read_bits(4));
    br.skip_bits(2);             // vps_base_layer_internal_flag + vps_base_layer_available_flag
    out.max_layers_minus1       = static_cast<uint8_t>(br.read_bits(6));
    out.max_sub_layers_minus1   = static_cast<uint8_t>(br.read_bits(3));
    out.temporal_id_nesting_flag = static_cast<uint8_t>(br.read_bits(1));
    br.skip_bits(16);            // vps_reserved_0xffff_16bits

    parse_profile_tier_level(br, true, out.max_sub_layers_minus1, out.ptl);

    const bool sub_layer_ordering_info_present = br.read_bit1();
    const int  start = sub_layer_ordering_info_present ? 0 : out.max_sub_layers_minus1;
    for (int i = start; i <= out.max_sub_layers_minus1 && i < kMaxSubLayers; ++i) {
        out.max_dec_pic_buffering_minus1[i] = br.read_ue();
        out.max_num_reorder_pics[i]         = br.read_ue();
        out.max_latency_increase_plus1[i]   = br.read_ue();
    }
    out.valid = !br.bad();
    return out.valid;
}

bool parse_sps(std::span<const uint8_t> rbsp, HevcSps& out) noexcept {
    BitReader br{rbsp.data(), rbsp.size()};

    out.vps_id                  = static_cast<uint8_t>(br.read_bits(4));
    out.max_sub_layers_minus1   = static_cast<uint8_t>(br.read_bits(3));
    out.temporal_id_nesting_flag = static_cast<uint8_t>(br.read_bits(1));

    parse_profile_tier_level(br, true, out.max_sub_layers_minus1, out.ptl);

    out.sps_id = static_cast<uint8_t>(br.read_ue());

    out.chroma_format_idc = static_cast<uint8_t>(br.read_ue());
    if (out.chroma_format_idc == 3) {
        out.separate_colour_plane_flag = static_cast<uint8_t>(br.read_bits(1));
    }
    out.pic_width_in_luma_samples  = br.read_ue();
    out.pic_height_in_luma_samples = br.read_ue();

    if (br.read_bit1()) {        // conformance_window_flag
        br.read_ue();            // conf_win_left_offset
        br.read_ue();            // conf_win_right_offset
        br.read_ue();            // conf_win_top_offset
        br.read_ue();            // conf_win_bottom_offset
    }

    out.bit_depth_y_minus8 = static_cast<uint8_t>(br.read_ue());
    out.bit_depth_c_minus8 = static_cast<uint8_t>(br.read_ue());

    out.log2_max_pic_order_cnt_lsb_minus4 = static_cast<uint8_t>(br.read_ue());

    out.sps_sub_layer_ordering_info_present_flag = static_cast<uint8_t>(br.read_bits(1));
    const int start = out.sps_sub_layer_ordering_info_present_flag ? 0 : out.max_sub_layers_minus1;
    for (int i = start; i <= out.max_sub_layers_minus1 && i < kMaxSubLayers; ++i) {
        out.max_dec_pic_buffering_minus1[i] = br.read_ue();
        out.max_num_reorder_pics[i]         = br.read_ue();
        out.max_latency_increase_plus1[i]   = br.read_ue();
    }

    out.log2_min_luma_coding_block_size_minus3 = br.read_ue();
    out.log2_diff_max_min_luma_coding_block_size = br.read_ue();
    out.log2_min_luma_transform_block_size_minus2 = br.read_ue();
    out.log2_diff_max_min_luma_transform_block_size = br.read_ue();
    out.max_transform_hierarchy_depth_inter = br.read_ue();
    out.max_transform_hierarchy_depth_intra = br.read_ue();

    out.scaling_list_enabled_flag = static_cast<uint8_t>(br.read_bits(1));
    if (out.scaling_list_enabled_flag) {
        if (br.read_bit1()) {     // sps_scaling_list_data_present_flag
            // §7.3.4 scaling_list_data — 跳过；v1 不用 scaling list（用默认）。
            // 解析需要 ~150 行；按 spec 的 size/matrix 逐表读 ue/se。这里直接放弃读取，
            // 因为 Hikvision IPC 主路径不发自定义 scaling list。
            // 未来回填：parse_scaling_list_data(br)。
            out.valid = false;
            return false;
        }
    }
    out.amp_enabled_flag = static_cast<uint8_t>(br.read_bits(1));
    out.sample_adaptive_offset_enabled_flag = static_cast<uint8_t>(br.read_bits(1));
    out.pcm_enabled_flag = static_cast<uint8_t>(br.read_bits(1));
    if (out.pcm_enabled_flag) {
        br.skip_bits(4 + 4);                  // pcm_sample_bit_depth_luma_minus1 + chroma_minus1
        br.read_ue();                          // log2_min_pcm_luma_coding_block_size_minus3
        br.read_ue();                          // log2_diff_max_min_pcm_luma_coding_block_size
        br.skip_bits(1);                       // pcm_loop_filter_disabled_flag
    }

    out.num_short_term_ref_pic_sets = static_cast<uint8_t>(br.read_ue());
    out.rps_list.assign(out.num_short_term_ref_pic_sets, ShortTermRefPicSet{});
    for (uint32_t i = 0; i < out.num_short_term_ref_pic_sets; ++i) {
        parse_st_rps(br, i, out.rps_list, out.rps_list[i]);
    }

    out.long_term_ref_pics_present_flag = static_cast<uint8_t>(br.read_bits(1));
    if (out.long_term_ref_pics_present_flag) {
        const uint32_t num_lt = br.read_ue();
        for (uint32_t i = 0; i < num_lt; ++i) {
            br.skip_bits(out.log2_max_pic_order_cnt_lsb_minus4 + 4);
            br.skip_bits(1);     // used_by_curr_pic_lt_sps_flag
        }
    }
    out.sps_temporal_mvp_enabled_flag      = static_cast<uint8_t>(br.read_bits(1));
    out.strong_intra_smoothing_enabled_flag = static_cast<uint8_t>(br.read_bits(1));

    out.vui_parameters_present_flag = static_cast<uint8_t>(br.read_bits(1));
    if (out.vui_parameters_present_flag) parse_vui(br, out);

    // 派生量
    out.min_cb_log2_size_y  = out.log2_min_luma_coding_block_size_minus3 + 3;
    out.ctb_log2_size_y     = out.min_cb_log2_size_y +
                                out.log2_diff_max_min_luma_coding_block_size;
    out.ctb_size_y          = 1u << out.ctb_log2_size_y;
    out.pic_width_in_ctbs   = (out.pic_width_in_luma_samples + out.ctb_size_y - 1) / out.ctb_size_y;
    out.pic_height_in_ctbs  = (out.pic_height_in_luma_samples + out.ctb_size_y - 1) / out.ctb_size_y;

    out.valid = !br.bad() &&
                  out.chroma_format_idc == 1 &&        // 仅 4:2:0
                  out.bit_depth_y_minus8 == 0 &&        // 仅 8-bit
                  out.bit_depth_c_minus8 == 0;
    return out.valid;
}

bool parse_pps(std::span<const uint8_t> rbsp, HevcPps& out, const HevcSps& /*sps*/) noexcept {
    BitReader br{rbsp.data(), rbsp.size()};

    out.pps_id = static_cast<uint8_t>(br.read_ue());
    out.sps_id = static_cast<uint8_t>(br.read_ue());

    out.dependent_slice_segments_enabled_flag = static_cast<uint8_t>(br.read_bits(1));
    out.output_flag_present_flag              = static_cast<uint8_t>(br.read_bits(1));
    out.num_extra_slice_header_bits           = static_cast<uint8_t>(br.read_bits(3));
    out.sign_data_hiding_enabled_flag         = static_cast<uint8_t>(br.read_bits(1));
    out.cabac_init_present_flag               = static_cast<uint8_t>(br.read_bits(1));
    out.num_ref_idx_l0_default_active_minus1  = br.read_ue();
    out.num_ref_idx_l1_default_active_minus1  = br.read_ue();
    out.init_qp_minus26                       = br.read_se();
    out.constrained_intra_pred_flag           = static_cast<uint8_t>(br.read_bits(1));
    out.transform_skip_enabled_flag           = static_cast<uint8_t>(br.read_bits(1));
    out.cu_qp_delta_enabled_flag              = static_cast<uint8_t>(br.read_bits(1));
    if (out.cu_qp_delta_enabled_flag) {
        out.diff_cu_qp_delta_depth = br.read_ue();
    }
    out.cb_qp_offset = br.read_se();
    out.cr_qp_offset = br.read_se();
    out.pps_slice_chroma_qp_offsets_present_flag = static_cast<uint8_t>(br.read_bits(1));
    out.weighted_pred_flag = static_cast<uint8_t>(br.read_bits(1));
    out.weighted_bipred_flag = static_cast<uint8_t>(br.read_bits(1));
    out.transquant_bypass_enabled_flag = static_cast<uint8_t>(br.read_bits(1));
    out.tiles_enabled_flag = static_cast<uint8_t>(br.read_bits(1));
    out.entropy_coding_sync_enabled_flag = static_cast<uint8_t>(br.read_bits(1));

    if (out.tiles_enabled_flag) {
        // v1 不支持 tiles。
        out.valid = false;
        return false;
    }

    out.pps_loop_filter_across_slices_enabled_flag = static_cast<uint8_t>(br.read_bits(1));
    out.deblocking_filter_control_present_flag = static_cast<uint8_t>(br.read_bits(1));
    if (out.deblocking_filter_control_present_flag) {
        out.deblocking_filter_override_enabled_flag = static_cast<uint8_t>(br.read_bits(1));
        out.pps_deblocking_filter_disabled_flag = static_cast<uint8_t>(br.read_bits(1));
        if (!out.pps_deblocking_filter_disabled_flag) {
            out.pps_beta_offset_div2 = br.read_se();
            out.pps_tc_offset_div2   = br.read_se();
        }
    }
    out.pps_scaling_list_data_present_flag = static_cast<uint8_t>(br.read_bits(1));
    if (out.pps_scaling_list_data_present_flag) {
        out.valid = false;     // 同 SPS 路径
        return false;
    }
    out.lists_modification_present_flag = static_cast<uint8_t>(br.read_bits(1));
    out.log2_parallel_merge_level_minus2 = br.read_ue();
    out.slice_segment_header_extension_present_flag = static_cast<uint8_t>(br.read_bits(1));

    out.valid = !br.bad();
    return out.valid;
}

bool parse_slice_header(std::span<const uint8_t> rbsp,
                         uint8_t                   nuh_type,
                         const HevcSps&            sps,
                         const HevcPps&            pps,
                         HevcSliceHeader&          out) noexcept {
    BitReader br{rbsp.data(), rbsp.size()};

    out.first_slice_segment_in_pic_flag = br.read_bit1();
    if (is_irap(nuh_type)) {
        out.no_output_of_prior_pics_flag = br.read_bit1();
    }
    out.pps_id = static_cast<uint8_t>(br.read_ue());

    if (!out.first_slice_segment_in_pic_flag) {
        if (pps.dependent_slice_segments_enabled_flag) {
            out.dependent_slice_segment_flag = br.read_bit1();
        }
        // slice_segment_address: ceil(log2(PicSizeInCtbsY)) bits
        uint32_t pic_ctbs = sps.pic_width_in_ctbs * sps.pic_height_in_ctbs;
        if (pic_ctbs == 0) pic_ctbs = 1;
        uint32_t bits = 0;
        while ((1u << bits) < pic_ctbs) ++bits;
        out.slice_segment_address = br.read_bits(bits);
    } else {
        out.slice_segment_address = 0;
    }

    if (!out.dependent_slice_segment_flag) {
        // num_extra_slice_header_bits 个保留 bit 跳过
        for (int i = 0; i < pps.num_extra_slice_header_bits; ++i) br.skip_bits(1);
        out.slice_type = static_cast<uint8_t>(br.read_ue());

        if (pps.output_flag_present_flag) br.skip_bits(1);  // pic_output_flag
        if (sps.separate_colour_plane_flag == 1) br.skip_bits(2);  // colour_plane_id

        if (!is_idr(nuh_type)) {
            out.slice_pic_order_cnt_lsb =
                static_cast<int32_t>(br.read_bits(sps.log2_max_pic_order_cnt_lsb_minus4 + 4));
            // short_term_ref_pic_set_sps_flag, ...：v1 IDR-only 路径不进
            // 完整实现见 Phase 4。
        } else {
            out.slice_pic_order_cnt_lsb = 0;
        }

        if (sps.sample_adaptive_offset_enabled_flag) {
            br.skip_bits(1);   // slice_sao_luma_flag
            if (sps.chroma_format_idc != 0) br.skip_bits(1);  // slice_sao_chroma_flag
        }

        // 进一步字段 (ref_pic_lists_modification, mvd_l1_zero_flag, cabac_init_flag 等) v1 暂不用
        // 这里直接读 slice_qp_delta（slice_type == I 时无 ref idx 修改路径会落到这里）。
        if (out.slice_type != 2 /*I*/) {
            // P/B slice — v1 暂不支持，但容错读 slice_qp_delta。
            out.valid = false;
            return false;
        }

        if (pps.cu_qp_delta_enabled_flag) {
            // qp_delta 在 slice header 里只是 slice 级别；CU 级在 slice data 里。
        }
        out.slice_qp_delta = br.read_se();
    }

    out.slice_qp = pps.init_qp_minus26 + 26 + out.slice_qp_delta;

    out.valid = !br.bad();
    return out.valid;
}

}  // namespace mclc
