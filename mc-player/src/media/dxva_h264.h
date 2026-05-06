/*
 * DXVA H.264 — codec_dxva_video.cpp 的 H.264 (AVC) 内联实装支持。
 *
 * 与 HEVC 路径并列：codec_dxva_video.cpp 仍是 D3D11VideoDevice / DPB / output pool /
 * worker thread / emit pipeline 的公共载体；本文件提供 H.264 私有部分：
 *   - NAL 类型常量、profile GUID
 *   - SPS / PPS / SliceHeader 最小 parser（仅取 DXVA picparams 所需字段）
 *   - DXVA_PicParams_H264 / DXVA_Slice_H264_Short 填表
 *   - POC 计算（pic_order_cnt_type = 0 / 2，type=1 当前不支持，IPC 流极少使用）
 *
 * 实装边界（v1，IPC 监控摄像机覆盖优先）：
 *   - chroma_format_idc=1 (4:2:0)，bit_depth=8
 *   - frame_mbs_only_flag=1（无 interlace / MBAFF / PAFF）
 *   - num_slice_groups_minus1=0（无 FMO/ASO）
 *   - pic_order_cnt_type ∈ {0, 2}（type=1 偏算法流，IPC 几乎不出现 → 拒绝）
 *   - 不支持自定义 scaling list（spec_default 规则）
 *   - 不支持 SVC / MVC NAL（subset SPS / prefix）
 *
 * 参考：
 *   - ITU-T Rec. H.264 (08/2021) §7.3.2 SPS/PPS/SliceHeader、§8.2.1 POC
 *   - Microsoft DXVA Spec for H.264 v1.1（DXVA_PicParams_H264 字段语义）
 *
 * 引用规则（CLAUDE.md / ADR-006 GPL 防污染）：
 *   仅参考 ITU-T 文本与 Microsoft DXVA spec；不读 OpenH264 / x264 / libavcodec 源码。
 */

#ifndef MC_PLAYER_MEDIA_DXVA_H264_H_
#define MC_PLAYER_MEDIA_DXVA_H264_H_

#include <Windows.h>
#include <dxva.h>
#include <initguid.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace mcp::media::h264 {

// ─── NAL unit types（H.264 §7.4.1）──────────────────────────────
constexpr uint8_t kNalSlice    = 1;
constexpr uint8_t kNalSliceDpA = 2;          // partition A，IPC 不出现
constexpr uint8_t kNalIdr      = 5;
constexpr uint8_t kNalSei      = 6;
constexpr uint8_t kNalSps      = 7;
constexpr uint8_t kNalPps      = 8;
constexpr uint8_t kNalAud      = 9;
constexpr uint8_t kNalEoSeq    = 10;
constexpr uint8_t kNalEoStream = 11;
constexpr uint8_t kNalFiller   = 12;

[[nodiscard]] constexpr bool is_idr(uint8_t t)         noexcept { return t == kNalIdr; }
[[nodiscard]] constexpr bool is_slice(uint8_t t)       noexcept { return t == kNalSlice || t == kNalIdr; }

// ─── SPS / PPS / SliceHeader（仅取 DXVA picparams 所需字段）─────
struct Sps {
    uint8_t  id                                 = 0;
    uint8_t  profile_idc                        = 0;
    uint8_t  constraint_set_flags               = 0;     // 6 bit constraint_set0..5
    uint8_t  level_idc                          = 0;
    uint8_t  chroma_format_idc                  = 1;     // 0=monochrome 1=4:2:0 2=4:2:2 3=4:4:4
    uint8_t  separate_colour_plane_flag         = 0;
    uint8_t  bit_depth_luma_minus8              = 0;
    uint8_t  bit_depth_chroma_minus8            = 0;
    uint8_t  qpprime_y_zero_transform_bypass_flag = 0;
    uint32_t log2_max_frame_num_minus4          = 0;
    uint8_t  pic_order_cnt_type                 = 0;
    uint32_t log2_max_pic_order_cnt_lsb_minus4  = 0;
    uint8_t  delta_pic_order_always_zero_flag   = 0;
    int32_t  offset_for_non_ref_pic             = 0;
    int32_t  offset_for_top_to_bottom_field     = 0;
    uint32_t num_ref_frames_in_pic_order_cnt_cycle = 0;
    std::array<int32_t, 256> offset_for_ref_frame{};
    uint8_t  num_ref_frames                     = 0;
    uint8_t  gaps_in_frame_num_value_allowed_flag = 0;
    uint32_t pic_width_in_mbs_minus1            = 0;
    uint32_t pic_height_in_map_units_minus1     = 0;
    uint8_t  frame_mbs_only_flag                = 1;
    uint8_t  mb_adaptive_frame_field_flag       = 0;
    uint8_t  direct_8x8_inference_flag          = 0;
    uint8_t  frame_cropping_flag                = 0;
    uint32_t frame_crop_left_offset             = 0;
    uint32_t frame_crop_right_offset            = 0;
    uint32_t frame_crop_top_offset              = 0;
    uint32_t frame_crop_bottom_offset           = 0;
    uint8_t  vui_parameters_present_flag        = 0;
    // VUI 子集（仅 colour 字段供 frame metadata 透传）
    uint8_t  video_full_range_flag              = 0;
    uint8_t  colour_description_present_flag    = 0;
    uint8_t  colour_primaries                   = 2;     // ISO/IEC 23001-8 unspecified
    uint8_t  transfer_characteristics           = 2;
    uint8_t  matrix_coefficients                = 2;
    uint8_t  bitstream_restriction_flag         = 0;
    uint32_t max_num_reorder_frames             = 0;
    bool     valid                              = false;
    // 派生
    uint32_t pic_width_in_samples_l             = 0;     // (mbs_minus1+1) * 16
    uint32_t pic_height_in_samples_l            = 0;     // (map_units_minus1+1) * 16 * (2-frame_mbs_only)
};

struct Pps {
    uint8_t  id                                 = 0;
    uint8_t  sps_id                             = 0;
    uint8_t  entropy_coding_mode_flag           = 0;     // 0=CAVLC 1=CABAC
    uint8_t  bottom_field_pic_order_in_frame_present_flag = 0;     // = pic_order_present_flag
    uint8_t  num_slice_groups_minus1            = 0;     // 仅支持 0
    uint8_t  num_ref_idx_l0_default_active_minus1 = 0;
    uint8_t  num_ref_idx_l1_default_active_minus1 = 0;
    uint8_t  weighted_pred_flag                 = 0;
    uint8_t  weighted_bipred_idc                = 0;
    int8_t   pic_init_qp_minus26                = 0;
    int8_t   pic_init_qs_minus26                = 0;
    int8_t   chroma_qp_index_offset             = 0;
    uint8_t  deblocking_filter_control_present_flag = 0;
    uint8_t  constrained_intra_pred_flag        = 0;
    uint8_t  redundant_pic_cnt_present_flag     = 0;
    // High profile 扩展（present 仅当 SPS profile 指示 High 系列且 PPS 段后有 trailing data）
    uint8_t  transform_8x8_mode_flag            = 0;
    uint8_t  pic_scaling_matrix_present_flag    = 0;     // v1 拒绝 = 1
    int8_t   second_chroma_qp_index_offset      = 0;
    bool     valid                              = false;
};

struct SliceHdr {
    uint32_t first_mb_in_slice                  = 0;
    uint32_t slice_type                         = 0;     // 0/5=P 1/6=B 2/7=I 3/8=SP 4/9=SI
    uint8_t  pps_id                             = 0;
    uint8_t  colour_plane_id                    = 0;
    uint32_t frame_num                          = 0;
    uint8_t  field_pic_flag                     = 0;
    uint8_t  bottom_field_flag                  = 0;
    uint32_t idr_pic_id                         = 0;
    uint32_t pic_order_cnt_lsb                  = 0;
    int32_t  delta_pic_order_cnt_bottom         = 0;
    std::array<int32_t, 2> delta_pic_order_cnt{};
    bool     valid                              = false;
};

// ─── 解析（PPS 解析需 SPS 决定 profile，SliceHeader 解析需 SPS+PPS）────
bool parse_sps(std::span<const uint8_t> rbsp, Sps& out) noexcept;
bool parse_pps(std::span<const uint8_t> rbsp, const Sps& sps, Pps& out) noexcept;
bool parse_slice_header_min(std::span<const uint8_t> rbsp,
                              uint8_t                  nal_type,
                              const Sps&               sps,
                              const Pps&               pps,
                              SliceHdr&                out) noexcept;

// ─── POC（H.264 §8.2.1）────────────────────────────────────────
// pic_order_cnt_type=0：lsb + msb 拼接（与 HEVC 类似）
// pic_order_cnt_type=2：POC = 2 * frame_num（IPC 单调递增 IPP 链典型）
// pic_order_cnt_type=1：v1 不支持 → 上层应拒绝
struct PocState {
    int32_t  prev_pic_order_cnt_msb         = 0;
    int32_t  prev_pic_order_cnt_lsb         = 0;
    uint32_t prev_frame_num                  = 0;
    uint32_t prev_frame_num_offset           = 0;
    bool     prev_pic_was_idr                = false;
    bool     prev_pic_was_ref                = false;
    bool     initialized                     = false;
};

int32_t compute_poc(PocState&                state,
                    const Sps&               sps,
                    const SliceHdr&          sh,
                    uint8_t                  nal_type,
                    uint8_t                  nal_ref_idc) noexcept;

// ─── DXVA picparams / slice 填表 ───────────────────────────────
// reference picture list 从 DPB 已激活的 short-term ref 中，按递减 frame_num 排序填入 RefFrameList。
// IPC 单 ref 流：仅前一张已解 P/IDR 帧作为 ref。
struct RefPic {
    uint8_t  dpb_index   = 0xFF;     // DPB slot index
    uint32_t frame_num   = 0;
    int32_t  field_order_cnt[2]{};   // [0]=top, [1]=bottom（progressive 两值相等）
    uint8_t  long_term   = 0;        // 0=short-term 1=long-term
    bool     used        = false;
};

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
                      uint16_t                     status_report_id) noexcept;

DXVA_Slice_H264_Short make_slice_short(uint32_t bs_offset, uint32_t bs_bytes) noexcept;

}  // namespace mcp::media::h264

#endif  // MC_PLAYER_MEDIA_DXVA_H264_H_
