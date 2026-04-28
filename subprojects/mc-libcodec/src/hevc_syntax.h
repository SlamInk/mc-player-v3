/*
 * HEVC syntax structures — VPS / SPS / PPS / Slice header（ITU-T H.265 §7.3.2 / §7.4.2）。
 *
 * 字段名严格对齐 spec，便于交叉对照。v1 仅支持 Main profile 4:2:0 8-bit，
 * 多余字段（scaling list、HRD、3D 扩展）解析但不使用。
 */

#ifndef MC_LIBCODEC_HEVC_SYNTAX_H_
#define MC_LIBCODEC_HEVC_SYNTAX_H_

#include <array>
#include <cstdint>
#include <vector>

#include "nal.h"

namespace mclc {

constexpr int kMaxSubLayers = 7;
constexpr int kMaxLongTermRefPics = 33;

struct ProfileTierLevel {
    uint8_t  general_profile_space;
    uint8_t  general_tier_flag;
    uint8_t  general_profile_idc;
    uint32_t general_profile_compatibility_flag;     // 32-bit mask
    uint8_t  general_level_idc;
    // sub_layer_*_present / level 略；v1 不用。
};

struct HrdParameters {
    // v1 占位，不解析。
    uint8_t present = 0;
};

struct ShortTermRefPicSet {
    int      num_negative_pics = 0;
    int      num_positive_pics = 0;
    int32_t  delta_poc_s0[16]{};
    int32_t  delta_poc_s1[16]{};
    uint8_t  used_by_curr_pic_s0[16]{};
    uint8_t  used_by_curr_pic_s1[16]{};
};

struct HevcVps {
    uint8_t  vps_id;
    uint8_t  max_layers_minus1;
    uint8_t  max_sub_layers_minus1;
    uint8_t  temporal_id_nesting_flag;
    ProfileTierLevel ptl;
    uint32_t max_dec_pic_buffering_minus1[kMaxSubLayers]{};
    uint32_t max_num_reorder_pics[kMaxSubLayers]{};
    uint32_t max_latency_increase_plus1[kMaxSubLayers]{};
    bool     valid = false;
};

struct HevcSps {
    uint8_t  sps_id;
    uint8_t  vps_id;
    uint8_t  max_sub_layers_minus1;
    uint8_t  temporal_id_nesting_flag;
    ProfileTierLevel ptl;

    uint8_t  chroma_format_idc;        // 1 = 4:2:0
    uint8_t  separate_colour_plane_flag;
    uint32_t pic_width_in_luma_samples;
    uint32_t pic_height_in_luma_samples;

    uint8_t  bit_depth_y_minus8;       // 0 = 8-bit
    uint8_t  bit_depth_c_minus8;
    uint8_t  log2_max_pic_order_cnt_lsb_minus4;

    uint8_t  sps_sub_layer_ordering_info_present_flag;
    uint32_t max_dec_pic_buffering_minus1[kMaxSubLayers]{};
    uint32_t max_num_reorder_pics[kMaxSubLayers]{};
    uint32_t max_latency_increase_plus1[kMaxSubLayers]{};

    uint32_t log2_min_luma_coding_block_size_minus3;
    uint32_t log2_diff_max_min_luma_coding_block_size;
    uint32_t log2_min_luma_transform_block_size_minus2;
    uint32_t log2_diff_max_min_luma_transform_block_size;
    uint32_t max_transform_hierarchy_depth_inter;
    uint32_t max_transform_hierarchy_depth_intra;

    uint8_t  scaling_list_enabled_flag;
    uint8_t  amp_enabled_flag;
    uint8_t  sample_adaptive_offset_enabled_flag;
    uint8_t  pcm_enabled_flag;
    uint8_t  long_term_ref_pics_present_flag;
    uint8_t  sps_temporal_mvp_enabled_flag;
    uint8_t  strong_intra_smoothing_enabled_flag;

    // 派生量
    uint32_t min_cb_log2_size_y;       // = log2_min_luma_coding_block_size_minus3 + 3
    uint32_t ctb_log2_size_y;          // = min_cb_log2_size_y + log2_diff_max
    uint32_t ctb_size_y;
    uint32_t pic_width_in_ctbs;
    uint32_t pic_height_in_ctbs;

    uint8_t  num_short_term_ref_pic_sets;
    std::vector<ShortTermRefPicSet> rps_list;     // num_short_term_ref_pic_sets 个

    // VUI 关键字段（VFR 元数据）。
    uint8_t  vui_parameters_present_flag;
    uint8_t  video_full_range_flag = 0;
    uint8_t  colour_description_present_flag = 0;
    uint8_t  colour_primaries = 2;                 // unspecified
    uint8_t  transfer_characteristics = 2;
    uint8_t  matrix_coeffs = 2;

    bool     valid = false;
};

struct HevcPps {
    uint8_t  pps_id;
    uint8_t  sps_id;
    uint8_t  dependent_slice_segments_enabled_flag;
    uint8_t  output_flag_present_flag;
    uint8_t  num_extra_slice_header_bits;
    uint8_t  sign_data_hiding_enabled_flag;
    uint8_t  cabac_init_present_flag;
    uint32_t num_ref_idx_l0_default_active_minus1;
    uint32_t num_ref_idx_l1_default_active_minus1;
    int32_t  init_qp_minus26;
    uint8_t  constrained_intra_pred_flag;
    uint8_t  transform_skip_enabled_flag;
    uint8_t  cu_qp_delta_enabled_flag;
    uint32_t diff_cu_qp_delta_depth;
    int32_t  cb_qp_offset;
    int32_t  cr_qp_offset;
    uint8_t  pps_slice_chroma_qp_offsets_present_flag;
    uint8_t  weighted_pred_flag;
    uint8_t  weighted_bipred_flag;
    uint8_t  transquant_bypass_enabled_flag;
    uint8_t  tiles_enabled_flag;
    uint8_t  entropy_coding_sync_enabled_flag;
    uint8_t  pps_loop_filter_across_slices_enabled_flag;
    uint8_t  deblocking_filter_control_present_flag;
    uint8_t  deblocking_filter_override_enabled_flag;
    uint8_t  pps_deblocking_filter_disabled_flag;
    int32_t  pps_beta_offset_div2;
    int32_t  pps_tc_offset_div2;
    uint8_t  pps_scaling_list_data_present_flag;
    uint8_t  lists_modification_present_flag;
    uint32_t log2_parallel_merge_level_minus2;
    uint8_t  slice_segment_header_extension_present_flag;
    bool     valid = false;
};

struct HevcSliceHeader {
    bool     first_slice_segment_in_pic_flag;
    bool     no_output_of_prior_pics_flag;
    uint8_t  pps_id;
    bool     dependent_slice_segment_flag;
    uint32_t slice_segment_address;
    uint8_t  slice_type;          // 0=B, 1=P, 2=I
    int32_t  slice_pic_order_cnt_lsb;
    int32_t  poc;                 // 派生
    int32_t  slice_qp_delta;
    int32_t  slice_qp;            // 派生 = pps.init_qp_minus26 + 26 + slice_qp_delta
    uint8_t  five_minus_max_num_merge_cand;
    bool     valid = false;
};

}  // namespace mclc

#endif  // MC_LIBCODEC_HEVC_SYNTAX_H_
