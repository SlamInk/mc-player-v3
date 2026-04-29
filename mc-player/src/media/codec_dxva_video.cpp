#include "media/codec_dxva_video.h"

#include <Windows.h>
#include <d3d11_1.h>
#include <dxva.h>
#include <initguid.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "pal/cache_line.h"
#include "pal/error.h"
#include "pal/log.h"
#include "pal/spsc_queue.h"
#include "pal/thread.h"

using Microsoft::WRL::ComPtr;

namespace mcp::media {

namespace {

// HEVC Main profile decoder GUID（与 dxgi_caps_probe 中一致）。
DEFINE_GUID(MCP_DXVA_HEVC_MAIN,
    0x5b11d51b, 0x2f4c, 0x4452, 0xbc, 0xc3, 0x09, 0xf2, 0xa1, 0x16, 0x0c, 0xc0);

constexpr uint8_t kNalTrailN     = 0;
constexpr uint8_t kNalIdrWRadl   = 19;
constexpr uint8_t kNalIdrNLp     = 20;
constexpr uint8_t kNalCraNut     = 21;
constexpr uint8_t kNalVpsNut     = 32;
constexpr uint8_t kNalSpsNut     = 33;
constexpr uint8_t kNalPpsNut     = 34;
constexpr uint8_t kNalAudNut     = 35;
constexpr uint8_t kNalPrefixSei  = 39;
constexpr uint8_t kNalSuffixSei  = 40;

constexpr uint8_t kInvalidIdx = 0xFF;     // DXVA RefPicList 不可用槽位

[[nodiscard]] constexpr bool is_irap(uint8_t t) noexcept { return t >= 16 && t <= 23; }
[[nodiscard]] constexpr bool is_idr(uint8_t t)  noexcept { return t == kNalIdrWRadl || t == kNalIdrNLp; }
[[nodiscard]] constexpr bool is_slice(uint8_t t) noexcept { return t <= 31; }

// ─── Bit reader（RBSP 比特流） ─────────────────────────────────
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
    uint32_t read_ue()   noexcept {
        uint32_t lz = 0;
        while (lz < 32 && pos_ < bits_) {
            if (read_bits(1) == 1) break;
            ++lz;
        }
        if (lz >= 32 || bad_) { bad_ = true; return 0; }
        const uint32_t suf = lz == 0 ? 0u : read_bits(lz);
        return (1u << lz) - 1u + suf;
    }
    int32_t read_se() noexcept {
        const uint32_t k = read_ue();
        return (k & 1u) ? static_cast<int32_t>((k + 1) >> 1) : -static_cast<int32_t>(k >> 1);
    }
    void skip_bits(uint32_t n) noexcept {
        if (pos_ + n > bits_) { bad_ = true; pos_ = bits_; return; }
        pos_ += n;
    }
    [[nodiscard]] std::size_t pos_bits() const noexcept { return pos_; }
    [[nodiscard]] bool bad() const noexcept { return bad_; }
private:
    const uint8_t* data_;
    std::size_t    bits_;
    std::size_t    pos_{0};
    bool           bad_{false};
};

// ─── 最小 HEVC 语法（仅 DXVA 所需字段） ───────────────────────
struct Vps {
    uint8_t  id{};
    uint8_t  max_sub_layers_minus1{};
    uint32_t max_dec_pic_buffering_minus1{};
};

struct Sps {
    uint8_t  id{};
    uint8_t  vps_id{};
    uint8_t  max_sub_layers_minus1{};
    uint8_t  chroma_format_idc{};
    uint8_t  separate_colour_plane_flag{};
    uint32_t pic_width_in_luma_samples{};
    uint32_t pic_height_in_luma_samples{};
    uint8_t  bit_depth_y_minus8{};
    uint8_t  bit_depth_c_minus8{};
    uint8_t  log2_max_pic_order_cnt_lsb_minus4{};
    uint32_t max_dec_pic_buffering_minus1{};
    uint32_t log2_min_cb_minus3{};
    uint32_t log2_diff_max_min_cb{};
    uint32_t log2_min_tb_minus2{};
    uint32_t log2_diff_max_min_tb{};
    uint32_t max_transform_hierarchy_depth_inter{};
    uint32_t max_transform_hierarchy_depth_intra{};
    uint8_t  scaling_list_enabled_flag{};
    uint8_t  amp_enabled_flag{};
    uint8_t  sample_adaptive_offset_enabled_flag{};
    uint8_t  pcm_enabled_flag{};
    uint8_t  pcm_sample_bit_depth_luma_minus1{};
    uint8_t  pcm_sample_bit_depth_chroma_minus1{};
    uint32_t log2_min_pcm_luma_cb_minus3{};
    uint32_t log2_diff_max_min_pcm_luma_cb{};
    uint8_t  pcm_loop_filter_disabled_flag{};
    uint8_t  num_short_term_ref_pic_sets{};
    uint8_t  num_long_term_ref_pics_sps{};
    uint8_t  long_term_ref_pics_present_flag{};
    uint8_t  sps_temporal_mvp_enabled_flag{};
    uint8_t  strong_intra_smoothing_enabled_flag{};
    // VUI colour fields
    uint8_t  vui_present{};
    uint8_t  video_full_range{};
    uint8_t  colour_desc_present{};
    uint8_t  colour_primaries{2};
    uint8_t  transfer_characteristics{2};
    uint8_t  matrix_coeffs{2};
    // 派生
    uint32_t ctb_size_y{};
    uint32_t pic_width_in_ctbs{};
    uint32_t pic_height_in_ctbs{};
    bool     valid{};
};

struct Pps {
    uint8_t  id{};
    uint8_t  sps_id{};
    uint8_t  dependent_slice_segments_enabled_flag{};
    uint8_t  output_flag_present_flag{};
    uint8_t  num_extra_slice_header_bits{};
    uint8_t  sign_data_hiding_enabled_flag{};
    uint8_t  cabac_init_present_flag{};
    uint8_t  num_ref_idx_l0_default_active_minus1{};
    uint8_t  num_ref_idx_l1_default_active_minus1{};
    int8_t   init_qp_minus26{};
    uint8_t  constrained_intra_pred_flag{};
    uint8_t  transform_skip_enabled_flag{};
    uint8_t  cu_qp_delta_enabled_flag{};
    uint8_t  diff_cu_qp_delta_depth{};
    int8_t   cb_qp_offset{};
    int8_t   cr_qp_offset{};
    uint8_t  pps_slice_chroma_qp_offsets_present_flag{};
    uint8_t  weighted_pred_flag{};
    uint8_t  weighted_bipred_flag{};
    uint8_t  transquant_bypass_enabled_flag{};
    uint8_t  tiles_enabled_flag{};
    uint8_t  entropy_coding_sync_enabled_flag{};
    uint8_t  pps_loop_filter_across_slices_enabled_flag{};
    uint8_t  deblocking_filter_override_enabled_flag{};
    uint8_t  pps_deblocking_filter_disabled_flag{};
    int8_t   pps_beta_offset_div2{};
    int8_t   pps_tc_offset_div2{};
    uint8_t  lists_modification_present_flag{};
    uint8_t  log2_parallel_merge_level_minus2{};
    uint8_t  slice_segment_header_extension_present_flag{};
    bool     valid{};
};

struct SliceHdr {
    bool     first_slice_segment_in_pic_flag{};
    bool     dependent_slice_segment_flag{};
    uint8_t  pps_id{};
    uint32_t slice_pic_order_cnt_lsb{};
    bool     valid{};
};

// ─── RBSP 抽取 + NAL 切分 ──────────────────────────────────────
void extract_rbsp(const uint8_t* ebsp, std::size_t n, std::vector<uint8_t>& out) noexcept {
    out.clear();
    out.reserve(n);
    for (std::size_t i = 0; i < n;) {
        if (i + 2 < n && ebsp[i] == 0 && ebsp[i + 1] == 0 && ebsp[i + 2] == 0x03) {
            out.push_back(0);
            out.push_back(0);
            i += 3;
        } else {
            out.push_back(ebsp[i]);
            ++i;
        }
    }
}

std::size_t find_start_code(std::span<const uint8_t> d, std::size_t from) noexcept {
    if (d.size() < 3) return d.size();
    for (std::size_t i = from; i + 2 < d.size(); ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) return i;
    }
    return d.size();
}

// 解析 profile_tier_level — 跳过大部分字段，只取 level_idc。
void parse_ptl(BitReader& br, int max_sub_layers_minus1) noexcept {
    br.skip_bits(2 + 1 + 5);   // profile_space + tier + profile_idc
    br.skip_bits(32);           // profile_compatibility_flag
    br.skip_bits(48);           // constraint flags
    br.skip_bits(8);            // level_idc
    uint8_t sub_prof[7]{};
    uint8_t sub_lvl[7]{};
    for (int i = 0; i < max_sub_layers_minus1; ++i) {
        sub_prof[i] = static_cast<uint8_t>(br.read_bits(1));
        sub_lvl[i]  = static_cast<uint8_t>(br.read_bits(1));
    }
    if (max_sub_layers_minus1 > 0) for (int i = max_sub_layers_minus1; i < 8; ++i) br.skip_bits(2);
    for (int i = 0; i < max_sub_layers_minus1; ++i) {
        if (sub_prof[i]) br.skip_bits(2 + 1 + 5 + 32 + 48);
        if (sub_lvl[i])  br.skip_bits(8);
    }
}

// 解析 short_term_ref_pic_set — 仅推进比特位置，不保存（DXVA 让驱动重解析）。
void parse_st_rps_skip(BitReader& br, uint32_t st_idx, int prev_count,
                        int* ref_neg_pos_count) noexcept {
    bool inter_pred = false;
    if (st_idx != 0) inter_pred = br.read_bit1();
    if (inter_pred) {
        if (static_cast<int>(st_idx) == prev_count) br.read_ue();    // delta_idx_minus1
        br.skip_bits(1);                                              // delta_rps_sign
        br.read_ue();                                                  // abs_delta_rps_minus1
        // num refs in previous set: caller must track
        const int nref = ref_neg_pos_count ? *ref_neg_pos_count : 0;
        for (int j = 0; j < nref + 1; ++j) {
            if (!br.read_bit1()) br.skip_bits(1);
        }
    } else {
        const int neg = static_cast<int>(br.read_ue());
        const int pos = static_cast<int>(br.read_ue());
        for (int i = 0; i < neg; ++i) {
            br.read_ue();         // delta_poc_s0_minus1
            br.skip_bits(1);     // used_by_curr_pic_s0
        }
        for (int i = 0; i < pos; ++i) {
            br.read_ue();
            br.skip_bits(1);
        }
        if (ref_neg_pos_count) *ref_neg_pos_count = neg + pos;
    }
}

void parse_vui_skip(BitReader& br, Sps& s) noexcept {
    if (br.read_bit1()) {                                // aspect_ratio_info_present_flag
        const uint8_t idc = static_cast<uint8_t>(br.read_bits(8));
        if (idc == 255) br.skip_bits(32);                // sar_width(16) + sar_height(16)
    }
    if (br.read_bit1()) br.skip_bits(1);                  // overscan_info_present + appropriate
    if (br.read_bit1()) {                                  // video_signal_type_present_flag
        br.skip_bits(3);                                   // video_format
        s.video_full_range    = static_cast<uint8_t>(br.read_bits(1));
        s.colour_desc_present = static_cast<uint8_t>(br.read_bits(1));
        if (s.colour_desc_present) {
            s.colour_primaries         = static_cast<uint8_t>(br.read_bits(8));
            s.transfer_characteristics = static_cast<uint8_t>(br.read_bits(8));
            s.matrix_coeffs            = static_cast<uint8_t>(br.read_bits(8));
        }
    }
    // 其余 VUI 字段（chroma_loc / neutral_chroma / field_seq / frame_field / default_display_window /
    // timing_info / hrd_parameters / bitstream_restriction）不再读取 — DXVA 不需要颜色之后的字段。
}

bool parse_vps(std::span<const uint8_t> rbsp, Vps& out) noexcept {
    BitReader br{rbsp.data(), rbsp.size()};
    out.id = static_cast<uint8_t>(br.read_bits(4));
    br.skip_bits(2);                                // base_layer_internal/available
    br.skip_bits(6);                                // max_layers_minus1
    out.max_sub_layers_minus1 = static_cast<uint8_t>(br.read_bits(3));
    br.skip_bits(1 + 16);                           // temporal_id_nesting + reserved
    parse_ptl(br, out.max_sub_layers_minus1);
    const bool ord = br.read_bit1();
    const int  start = ord ? 0 : out.max_sub_layers_minus1;
    for (int i = start; i <= out.max_sub_layers_minus1 && i < 7; ++i) {
        const uint32_t mdpb = br.read_ue();
        br.read_ue();                                // max_num_reorder_pics
        br.read_ue();                                // max_latency_increase_plus1
        if (i == out.max_sub_layers_minus1) out.max_dec_pic_buffering_minus1 = mdpb;
    }
    return !br.bad();
}

bool parse_sps(std::span<const uint8_t> rbsp, Sps& out) noexcept {
    BitReader br{rbsp.data(), rbsp.size()};
    out.vps_id                = static_cast<uint8_t>(br.read_bits(4));
    out.max_sub_layers_minus1 = static_cast<uint8_t>(br.read_bits(3));
    br.skip_bits(1);                                 // temporal_id_nesting_flag
    parse_ptl(br, out.max_sub_layers_minus1);
    out.id = static_cast<uint8_t>(br.read_ue());
    out.chroma_format_idc = static_cast<uint8_t>(br.read_ue());
    if (out.chroma_format_idc == 3) out.separate_colour_plane_flag = static_cast<uint8_t>(br.read_bits(1));
    out.pic_width_in_luma_samples  = br.read_ue();
    out.pic_height_in_luma_samples = br.read_ue();
    if (br.read_bit1()) {                            // conformance_window_flag
        br.read_ue(); br.read_ue(); br.read_ue(); br.read_ue();
    }
    out.bit_depth_y_minus8 = static_cast<uint8_t>(br.read_ue());
    out.bit_depth_c_minus8 = static_cast<uint8_t>(br.read_ue());
    out.log2_max_pic_order_cnt_lsb_minus4 = static_cast<uint8_t>(br.read_ue());

    const bool ord = br.read_bit1();
    const int  start = ord ? 0 : out.max_sub_layers_minus1;
    for (int i = start; i <= out.max_sub_layers_minus1 && i < 7; ++i) {
        const uint32_t mdpb = br.read_ue();
        br.read_ue();
        br.read_ue();
        if (i == out.max_sub_layers_minus1) out.max_dec_pic_buffering_minus1 = mdpb;
    }
    out.log2_min_cb_minus3      = br.read_ue();
    out.log2_diff_max_min_cb    = br.read_ue();
    out.log2_min_tb_minus2      = br.read_ue();
    out.log2_diff_max_min_tb    = br.read_ue();
    out.max_transform_hierarchy_depth_inter = br.read_ue();
    out.max_transform_hierarchy_depth_intra = br.read_ue();
    out.scaling_list_enabled_flag = static_cast<uint8_t>(br.read_bits(1));
    if (out.scaling_list_enabled_flag) {
        if (br.read_bit1()) {                        // sps_scaling_list_data_present
            // v1 不支持自定义 scaling list — 但摄像机一般也不发。
            return false;
        }
    }
    out.amp_enabled_flag = static_cast<uint8_t>(br.read_bits(1));
    out.sample_adaptive_offset_enabled_flag = static_cast<uint8_t>(br.read_bits(1));
    out.pcm_enabled_flag = static_cast<uint8_t>(br.read_bits(1));
    if (out.pcm_enabled_flag) {
        out.pcm_sample_bit_depth_luma_minus1   = static_cast<uint8_t>(br.read_bits(4));
        out.pcm_sample_bit_depth_chroma_minus1 = static_cast<uint8_t>(br.read_bits(4));
        out.log2_min_pcm_luma_cb_minus3        = br.read_ue();
        out.log2_diff_max_min_pcm_luma_cb      = br.read_ue();
        out.pcm_loop_filter_disabled_flag       = static_cast<uint8_t>(br.read_bits(1));
    }
    out.num_short_term_ref_pic_sets = static_cast<uint8_t>(br.read_ue());
    int prev_count = 0;
    for (uint32_t i = 0; i < out.num_short_term_ref_pic_sets; ++i) {
        parse_st_rps_skip(br, i, prev_count, &prev_count);
    }
    out.long_term_ref_pics_present_flag = static_cast<uint8_t>(br.read_bits(1));
    if (out.long_term_ref_pics_present_flag) {
        out.num_long_term_ref_pics_sps = static_cast<uint8_t>(br.read_ue());
        for (int i = 0; i < out.num_long_term_ref_pics_sps; ++i) {
            br.skip_bits(out.log2_max_pic_order_cnt_lsb_minus4 + 4);
            br.skip_bits(1);
        }
    }
    out.sps_temporal_mvp_enabled_flag       = static_cast<uint8_t>(br.read_bits(1));
    out.strong_intra_smoothing_enabled_flag = static_cast<uint8_t>(br.read_bits(1));
    out.vui_present = static_cast<uint8_t>(br.read_bits(1));
    if (out.vui_present) parse_vui_skip(br, out);

    // 派生量
    const uint32_t min_cb = out.log2_min_cb_minus3 + 3;
    const uint32_t ctb_log2 = min_cb + out.log2_diff_max_min_cb;
    out.ctb_size_y         = 1u << ctb_log2;
    out.pic_width_in_ctbs  = (out.pic_width_in_luma_samples + out.ctb_size_y - 1) / out.ctb_size_y;
    out.pic_height_in_ctbs = (out.pic_height_in_luma_samples + out.ctb_size_y - 1) / out.ctb_size_y;
    out.valid = !br.bad() &&
                  out.chroma_format_idc == 1 &&
                  out.bit_depth_y_minus8 == 0 &&
                  out.bit_depth_c_minus8 == 0;
    return out.valid;
}

bool parse_pps(std::span<const uint8_t> rbsp, Pps& out) noexcept {
    BitReader br{rbsp.data(), rbsp.size()};
    out.id     = static_cast<uint8_t>(br.read_ue());
    out.sps_id = static_cast<uint8_t>(br.read_ue());
    out.dependent_slice_segments_enabled_flag = static_cast<uint8_t>(br.read_bits(1));
    out.output_flag_present_flag              = static_cast<uint8_t>(br.read_bits(1));
    out.num_extra_slice_header_bits           = static_cast<uint8_t>(br.read_bits(3));
    out.sign_data_hiding_enabled_flag         = static_cast<uint8_t>(br.read_bits(1));
    out.cabac_init_present_flag               = static_cast<uint8_t>(br.read_bits(1));
    out.num_ref_idx_l0_default_active_minus1  = static_cast<uint8_t>(br.read_ue());
    out.num_ref_idx_l1_default_active_minus1  = static_cast<uint8_t>(br.read_ue());
    out.init_qp_minus26                       = static_cast<int8_t>(br.read_se());
    out.constrained_intra_pred_flag           = static_cast<uint8_t>(br.read_bits(1));
    out.transform_skip_enabled_flag           = static_cast<uint8_t>(br.read_bits(1));
    out.cu_qp_delta_enabled_flag              = static_cast<uint8_t>(br.read_bits(1));
    if (out.cu_qp_delta_enabled_flag) {
        out.diff_cu_qp_delta_depth = static_cast<uint8_t>(br.read_ue());
    }
    out.cb_qp_offset = static_cast<int8_t>(br.read_se());
    out.cr_qp_offset = static_cast<int8_t>(br.read_se());
    out.pps_slice_chroma_qp_offsets_present_flag = static_cast<uint8_t>(br.read_bits(1));
    out.weighted_pred_flag        = static_cast<uint8_t>(br.read_bits(1));
    out.weighted_bipred_flag      = static_cast<uint8_t>(br.read_bits(1));
    out.transquant_bypass_enabled_flag = static_cast<uint8_t>(br.read_bits(1));
    out.tiles_enabled_flag        = static_cast<uint8_t>(br.read_bits(1));
    out.entropy_coding_sync_enabled_flag = static_cast<uint8_t>(br.read_bits(1));
    if (out.tiles_enabled_flag) return false;     // v1 不支持 tiles
    out.pps_loop_filter_across_slices_enabled_flag = static_cast<uint8_t>(br.read_bits(1));
    if (br.read_bit1()) {                              // deblocking_filter_control_present
        out.deblocking_filter_override_enabled_flag = static_cast<uint8_t>(br.read_bits(1));
        out.pps_deblocking_filter_disabled_flag     = static_cast<uint8_t>(br.read_bits(1));
        if (!out.pps_deblocking_filter_disabled_flag) {
            out.pps_beta_offset_div2 = static_cast<int8_t>(br.read_se());
            out.pps_tc_offset_div2   = static_cast<int8_t>(br.read_se());
        }
    }
    if (br.read_bit1()) return false;                  // pps_scaling_list_data_present — v1 不支持
    out.lists_modification_present_flag = static_cast<uint8_t>(br.read_bits(1));
    out.log2_parallel_merge_level_minus2 = static_cast<uint8_t>(br.read_ue());
    out.slice_segment_header_extension_present_flag = static_cast<uint8_t>(br.read_bits(1));
    out.valid = !br.bad();
    return out.valid;
}

bool parse_slice_header_min(std::span<const uint8_t> rbsp,
                              uint8_t                   nuh_type,
                              const Sps&                sps,
                              const Pps&                pps,
                              SliceHdr&                 out) noexcept {
    BitReader br{rbsp.data(), rbsp.size()};
    out.first_slice_segment_in_pic_flag = br.read_bit1();
    if (is_irap(nuh_type)) br.skip_bits(1);          // no_output_of_prior_pics_flag
    out.pps_id = static_cast<uint8_t>(br.read_ue());

    if (!out.first_slice_segment_in_pic_flag) {
        if (pps.dependent_slice_segments_enabled_flag) {
            out.dependent_slice_segment_flag = br.read_bit1();
        }
        uint32_t pic_ctbs = sps.pic_width_in_ctbs * sps.pic_height_in_ctbs;
        if (pic_ctbs == 0) pic_ctbs = 1;
        uint32_t bits = 0;
        while ((1u << bits) < pic_ctbs) ++bits;
        br.skip_bits(bits);                          // slice_segment_address
    }

    if (!out.dependent_slice_segment_flag) {
        for (int i = 0; i < pps.num_extra_slice_header_bits; ++i) br.skip_bits(1);
        br.read_ue();                                // slice_type
        if (pps.output_flag_present_flag) br.skip_bits(1);
        if (sps.separate_colour_plane_flag == 1) br.skip_bits(2);
        if (!is_idr(nuh_type)) {
            out.slice_pic_order_cnt_lsb = br.read_bits(sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
        }
    }
    out.valid = !br.bad();
    return out.valid;
}

}  // namespace (anonymous)

// ─── Impl ──────────────────────────────────────────────────────
struct CodecDxvaVideo::Impl {
    Config                                    cfg;
    ComPtr<ID3D11VideoDevice>                 video_device;
    ComPtr<ID3D11VideoContext>                video_ctx;
    ComPtr<ID3D11DeviceContext>               ctx;
    ComPtr<ID3D11VideoDecoder>                decoder;
    GUID                                       profile = MCP_DXVA_HEVC_MAIN;
    D3D11_VIDEO_DECODER_CONFIG                cfg_used{};
    UINT                                       cfg_bitstream_raw = 1;     // 1 = Annex-B as-is

    // 参数集缓存
    std::unordered_map<uint8_t, Vps> vps;
    std::unordered_map<uint8_t, Sps> sps;
    std::unordered_map<uint8_t, Pps> pps;
    bool                              decoder_inited = false;
    uint32_t                          dec_w = 0, dec_h = 0;

    // DPB pool（BIND_DECODER + ID3D11VideoDecoderOutputView）
    ComPtr<ID3D11Texture2D>                                 dpb_tex;
    std::vector<ComPtr<ID3D11VideoDecoderOutputView>>       dpb_views;
    UINT                                                     dpb_size = 8;

    struct DpbEntry {
        bool    used = false;
        int32_t poc = 0;
        bool    output_pending = false;
    };
    std::vector<DpbEntry> dpb;
    int32_t prev_poc_msb = 0;
    int32_t prev_poc_lsb = 0;
    // single-ref P-slice tracking（IPP 低延时流的简化 RPS）
    UINT    last_ref_dpb_idx = UINT_MAX;
    int32_t last_ref_poc     = 0;

    // 输出 pool（BIND_SHADER_RESOURCE，喂 Renderer）
    ComPtr<ID3D11Texture2D> out_pool;
    UINT                    out_pool_size = 4;
    UINT                    out_next       = 0;
    bool                    out_pool_array_capable = true;

    // 创建 + 烟测：对 NV12 用 R8/R8G8 plane SRV 试一次，避免 SetTexture 成功但 SRV 失败的驱动陷阱。
    bool try_create_out_pool(const D3D11_TEXTURE2D_DESC& desc) noexcept {
        ComPtr<ID3D11Texture2D> tex;
        HRESULT hr = cfg.device->CreateTexture2D(&desc, nullptr, &tex);
        if (FAILED(hr) || !tex) return false;
        // 烟测：试创建 plane SRV，失败说明 driver 对该组合不支持 SR 路径。
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R8_UNORM;
        if (desc.ArraySize > 1) {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            sd.Texture2DArray.MipLevels       = 1;
            sd.Texture2DArray.FirstArraySlice = 0;
            sd.Texture2DArray.ArraySize       = 1;
        } else {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
        }
        ComPtr<ID3D11ShaderResourceView> probe_srv;
        hr = cfg.device->CreateShaderResourceView(tex.Get(), &sd, &probe_srv);
        if (FAILED(hr)) {
            MCP_LOGF(pal::LogLevel::warn,
                     "CodecDxvaVideo: out_pool SRV smoke-test failed bind=0x%X arr=%u hr=0x%08lX",
                     desc.BindFlags, desc.ArraySize, hr);
            return false;
        }
        out_pool = tex;
        return true;
    }

    // 当前 AU 累积
    std::vector<uint8_t>                 bitstream_buf;
    std::vector<DXVA_Slice_HEVC_Short>    slice_ctrl;
    bool                                  has_pic = false;
    int32_t                               cur_poc = 0;
    UINT                                  cur_dpb_idx = 0;
    uint8_t                               cur_nal_type = 0;
    int64_t                               cur_pts_us = 0;
    int64_t                               cur_arrival_qpc_ns = 0;     // 端到端延时探针起点
    Sps*                                  cur_sps = nullptr;
    Pps*                                  cur_pps = nullptr;

    std::vector<uint8_t>                 scratch_rbsp;

    // ─── T4-DXVA worker（ADD §3.3）─────────────────────────────────
    // 原版 submit 同步在 RX (T2) 线程跑 NAL 解析 + DPB 管理 + DXVA EndFrame，
    // 占用 TIME_CRITICAL + P-core 的 socket 线程几 ms，期间 socket 内核缓冲堆积。
    // 现在拆到独立 worker：submit 仅入队，T4 worker 取出再 on_au。
    struct PendingAu {
        std::vector<uint8_t> bytes;
        int64_t              pts_us         = 0;
        int64_t              arrival_qpc_ns = 0;
    };
    // SPSC ring（power-of-2，CLAUDE.md / ADD §6.1 硬约束）。容量 4 = anchor + 2-3 P
    // ~130 ms@30fps；producer 满时丢自己（drop-newest），consumer 在 try_pop 前
    // try_drop_oldest 到只剩 1 维持"最新优先"语义。与 codec_mft_video.cpp 对齐。
    static constexpr std::size_t kAuQueueCap = 4;

    std::thread                          worker_thread;
    std::atomic<bool>                    worker_stop{false};
    alignas(pal::kCacheLineSize) std::mutex                au_mu;
    alignas(pal::kCacheLineSize) std::condition_variable   au_cv;
    alignas(pal::kCacheLineSize) pal::SpscQueue<PendingAu> au_queue{kAuQueueCap};

    void worker_loop() noexcept;

    // ── lifecycle ──
    HRESULT init_video_device() noexcept {
        HRESULT hr = cfg.device.As(&video_device);
        if (FAILED(hr)) return hr;
        cfg.device->GetImmediateContext(&ctx);
        return ctx.As(&video_ctx);
    }

    HRESULT pick_decoder_config(const Sps& s) noexcept {
        D3D11_VIDEO_DECODER_DESC desc{};
        desc.Guid         = profile;
        desc.SampleWidth  = s.pic_width_in_luma_samples;
        desc.SampleHeight = s.pic_height_in_luma_samples;
        desc.OutputFormat = DXGI_FORMAT_NV12;
        UINT n = 0;
        HRESULT hr = video_device->GetVideoDecoderConfigCount(&desc, &n);
        if (FAILED(hr) || n == 0) return E_FAIL;
        for (UINT i = 0; i < n; ++i) {
            D3D11_VIDEO_DECODER_CONFIG c{};
            if (SUCCEEDED(video_device->GetVideoDecoderConfig(&desc, i, &c))) {
                if (c.ConfigBitstreamRaw == 1) {     // Annex-B with start codes
                    cfg_used = c;
                    cfg_bitstream_raw = 1;
                    return S_OK;
                }
            }
        }
        // 降级：取首个支持的 config（可能要求 stripped 格式）。
        return video_device->GetVideoDecoderConfig(&desc, 0, &cfg_used);
    }

    HRESULT init_decoder(const Sps& s) noexcept {
        if (decoder_inited) return S_OK;
        HRESULT hr = pick_decoder_config(s);
        if (FAILED(hr)) {
            MCP_LOGF(pal::LogLevel::error,
                     "CodecDxvaVideo: GetVideoDecoderConfig failed hr=0x%08lX", hr);
            return hr;
        }
        D3D11_VIDEO_DECODER_DESC desc{};
        desc.Guid         = profile;
        desc.SampleWidth  = s.pic_width_in_luma_samples;
        desc.SampleHeight = s.pic_height_in_luma_samples;
        desc.OutputFormat = DXGI_FORMAT_NV12;
        hr = video_device->CreateVideoDecoder(&desc, &cfg_used, &decoder);
        if (FAILED(hr)) {
            MCP_LOGF(pal::LogLevel::error,
                     "CodecDxvaVideo: CreateVideoDecoder failed hr=0x%08lX", hr);
            return hr;
        }
        dec_w = s.pic_width_in_luma_samples;
        dec_h = s.pic_height_in_luma_samples;
        // DPB 大小：sps 给的 + 1（current）+ 安全裕度。最少要满足 driver 报的 ConfigMinRenderTargetBuffCount。
        dpb_size = std::max<UINT>(s.max_dec_pic_buffering_minus1 + 2, 4);
        if (cfg_used.ConfigMinRenderTargetBuffCount > dpb_size) {
            dpb_size = cfg_used.ConfigMinRenderTargetBuffCount;
        }
        if (dpb_size > 20) dpb_size = 20;

        // DPB texture array（BIND_DECODER）。NV12 dim 必须偶数。driver 写入端无需 SR bind。
        const UINT tex_w = (dec_w + 1u) & ~1u;
        const UINT tex_h = (dec_h + 1u) & ~1u;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = tex_w; td.Height = tex_h;
        td.MipLevels = 1; td.ArraySize = dpb_size;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_DECODER;
        hr = cfg.device->CreateTexture2D(&td, nullptr, &dpb_tex);
        if (FAILED(hr)) {
            MCP_LOGF(pal::LogLevel::error,
                     "CodecDxvaVideo: DPB CreateTexture2D failed hr=0x%08lX %ux%u arr=%u",
                     hr, tex_w, tex_h, dpb_size);
            return hr;
        }

        dpb_views.resize(dpb_size);
        for (UINT i = 0; i < dpb_size; ++i) {
            D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC vd{};
            vd.DecodeProfile           = profile;
            vd.ViewDimension           = D3D11_VDOV_DIMENSION_TEXTURE2D;
            vd.Texture2D.ArraySlice    = i;
            hr = video_device->CreateVideoDecoderOutputView(
                    dpb_tex.Get(), &vd, &dpb_views[i]);
            if (FAILED(hr)) {
                MCP_LOGF(pal::LogLevel::error,
                         "CodecDxvaVideo: CreateVideoDecoderOutputView slice=%u hr=0x%08lX",
                         i, hr);
                return hr;
            }
        }
        dpb.assign(dpb_size, DpbEntry{});

        // 输出 pool — 创建一定要支持 SRV，否则 render_d3d11 无法采样视频。
        // AMD 独显（含 RX 7000 系列）driver 对 NV12 + BIND_DECODER + ArraySize>1
        // 的 BIND_SHADER_RESOURCE 路径在创建 SRV 时报 E_INVALIDARG，导致渲染端
        // 静默丢帧。所以用预飞行测试探测能否真的拿到 SRV，挑选第一个能过的方案：
        //   ① dual-bind + ArraySize=N
        //   ② SR-only + ArraySize=N（去 DECODER bit；不少 AMD/NV 驱动接受 NV12 SR 阵列）
        //   ③ SR-only + ArraySize=1（最兼容兜底）
        out_pool_array_capable = true;
        D3D11_TEXTURE2D_DESC ot = td;
        ot.ArraySize = out_pool_size;
        ot.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
        bool ok = try_create_out_pool(ot);
        if (!ok) {
            MCP_LOGF(pal::LogLevel::warn,
                     "CodecDxvaVideo: out_pool dual-bind+arr=%u failed; retry SR-only arr=%u",
                     out_pool_size, out_pool_size);
            ot = td;
            ot.ArraySize = out_pool_size;
            ot.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ok = try_create_out_pool(ot);
        }
        if (!ok) {
            MCP_LOGF(pal::LogLevel::warn,
                     "CodecDxvaVideo: out_pool SR-only arr=%u failed; retry SR-only arr=1",
                     out_pool_size);
            out_pool_size = 1;
            out_pool_array_capable = false;
            ot = td;
            ot.ArraySize = 1;
            ot.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ok = try_create_out_pool(ot);
        }
        if (!ok) {
            MCP_LOGF(pal::LogLevel::error,
                     "CodecDxvaVideo: out_pool 全部 fallback 失败 %ux%u", tex_w, tex_h);
            return E_FAIL;
        }
        MCP_LOGF(pal::LogLevel::info,
                 "CodecDxvaVideo: out_pool ok bind=0x%X arr=%u",
                 ot.BindFlags, ot.ArraySize);

        decoder_inited = true;
        MCP_LOGF(pal::LogLevel::info,
                 "CodecDxvaVideo: decoder ready %ux%u dpb=%u (Annex-B raw=%u)",
                 dec_w, dec_h, dpb_size, cfg_bitstream_raw);
        return S_OK;
    }

    UINT alloc_dpb_slot() noexcept {
        for (UINT i = 0; i < dpb_size; ++i) if (!dpb[i].used) return i;
        // 全满：覆盖最旧（POC 最小），但不能覆盖 last_ref（still in use as P-slice 参考）。
        UINT    victim  = UINT_MAX;
        int32_t min_poc = INT32_MAX;
        for (UINT i = 0; i < dpb_size; ++i) {
            if (i == last_ref_dpb_idx) continue;
            if (dpb[i].poc < min_poc) { min_poc = dpb[i].poc; victim = i; }
        }
        if (victim == UINT_MAX) victim = 0;            // 防御：只有 last_ref 占着的退化情形
        dpb[victim] = DpbEntry{};
        return victim;
    }

    // 计算 POC（§8.3.1）
    int32_t compute_poc(uint8_t nuh_type, uint32_t poc_lsb, uint32_t max_poc_lsb_log2) noexcept {
        if (is_idr(nuh_type)) {
            prev_poc_msb = 0;
            prev_poc_lsb = 0;
            return 0;
        }
        const int32_t max_poc_lsb = 1 << (max_poc_lsb_log2 + 4);
        int32_t poc_msb = 0;
        if (static_cast<int32_t>(poc_lsb) < prev_poc_lsb &&
            (prev_poc_lsb - static_cast<int32_t>(poc_lsb)) >= max_poc_lsb / 2) {
            poc_msb = prev_poc_msb + max_poc_lsb;
        } else if (static_cast<int32_t>(poc_lsb) > prev_poc_lsb &&
                   (static_cast<int32_t>(poc_lsb) - prev_poc_lsb) > max_poc_lsb / 2) {
            poc_msb = prev_poc_msb - max_poc_lsb;
        } else {
            poc_msb = prev_poc_msb;
        }
        const int32_t poc = poc_msb + static_cast<int32_t>(poc_lsb);
        prev_poc_msb = poc_msb;
        prev_poc_lsb = poc_lsb;
        return poc;
    }

    // 填充 DXVA_PicParams_HEVC（§DXVA HEVC spec）
    void fill_pic_params(DXVA_PicParams_HEVC& pp,
                          const Sps& s, const Pps& p,
                          uint8_t nuh_type, int32_t poc, UINT dpb_idx) noexcept {
        std::memset(&pp, 0, sizeof(pp));
        pp.PicWidthInMinCbsY  = static_cast<USHORT>(
            s.pic_width_in_luma_samples >> (s.log2_min_cb_minus3 + 3));
        pp.PicHeightInMinCbsY = static_cast<USHORT>(
            s.pic_height_in_luma_samples >> (s.log2_min_cb_minus3 + 3));
        pp.chroma_format_idc           = s.chroma_format_idc;
        pp.separate_colour_plane_flag  = s.separate_colour_plane_flag;
        pp.bit_depth_luma_minus8       = s.bit_depth_y_minus8;
        pp.bit_depth_chroma_minus8     = s.bit_depth_c_minus8;
        pp.log2_max_pic_order_cnt_lsb_minus4 = s.log2_max_pic_order_cnt_lsb_minus4;
        pp.NoPicReorderingFlag = 0;
        pp.NoBiPredFlag        = 0;
        pp.CurrPic.Index7Bits      = static_cast<UCHAR>(dpb_idx);
        pp.CurrPic.AssociatedFlag  = 0;
        pp.sps_max_dec_pic_buffering_minus1 = static_cast<UCHAR>(s.max_dec_pic_buffering_minus1);
        pp.log2_min_luma_coding_block_size_minus3   = static_cast<UCHAR>(s.log2_min_cb_minus3);
        pp.log2_diff_max_min_luma_coding_block_size = static_cast<UCHAR>(s.log2_diff_max_min_cb);
        pp.log2_min_transform_block_size_minus2     = static_cast<UCHAR>(s.log2_min_tb_minus2);
        pp.log2_diff_max_min_transform_block_size   = static_cast<UCHAR>(s.log2_diff_max_min_tb);
        pp.max_transform_hierarchy_depth_inter = static_cast<UCHAR>(s.max_transform_hierarchy_depth_inter);
        pp.max_transform_hierarchy_depth_intra = static_cast<UCHAR>(s.max_transform_hierarchy_depth_intra);
        pp.num_short_term_ref_pic_sets   = s.num_short_term_ref_pic_sets;
        pp.num_long_term_ref_pics_sps    = s.num_long_term_ref_pics_sps;
        pp.num_ref_idx_l0_default_active_minus1 = p.num_ref_idx_l0_default_active_minus1;
        pp.num_ref_idx_l1_default_active_minus1 = p.num_ref_idx_l1_default_active_minus1;
        pp.init_qp_minus26 = p.init_qp_minus26;
        pp.ucNumDeltaPocsOfRefRpsIdx = 0;
        pp.wNumBitsForShortTermRPSInSlice = 0;

        // Coding tool flags
        pp.scaling_list_enabled_flag = s.scaling_list_enabled_flag;
        pp.amp_enabled_flag          = s.amp_enabled_flag;
        pp.sample_adaptive_offset_enabled_flag = s.sample_adaptive_offset_enabled_flag;
        pp.pcm_enabled_flag          = s.pcm_enabled_flag;
        pp.pcm_sample_bit_depth_luma_minus1   = s.pcm_sample_bit_depth_luma_minus1;
        pp.pcm_sample_bit_depth_chroma_minus1 = s.pcm_sample_bit_depth_chroma_minus1;
        pp.log2_min_pcm_luma_coding_block_size_minus3 = static_cast<UINT32>(s.log2_min_pcm_luma_cb_minus3);
        pp.log2_diff_max_min_pcm_luma_coding_block_size = static_cast<UINT32>(s.log2_diff_max_min_pcm_luma_cb);
        pp.pcm_loop_filter_disabled_flag = s.pcm_loop_filter_disabled_flag;
        pp.long_term_ref_pics_present_flag = s.long_term_ref_pics_present_flag;
        pp.sps_temporal_mvp_enabled_flag = s.sps_temporal_mvp_enabled_flag;
        pp.strong_intra_smoothing_enabled_flag = s.strong_intra_smoothing_enabled_flag;
        pp.dependent_slice_segments_enabled_flag = p.dependent_slice_segments_enabled_flag;
        pp.output_flag_present_flag = p.output_flag_present_flag;
        pp.num_extra_slice_header_bits = p.num_extra_slice_header_bits;
        pp.sign_data_hiding_enabled_flag = p.sign_data_hiding_enabled_flag;
        pp.cabac_init_present_flag = p.cabac_init_present_flag;

        // Picture property flags
        pp.constrained_intra_pred_flag = p.constrained_intra_pred_flag;
        pp.transform_skip_enabled_flag = p.transform_skip_enabled_flag;
        pp.cu_qp_delta_enabled_flag    = p.cu_qp_delta_enabled_flag;
        pp.pps_slice_chroma_qp_offsets_present_flag = p.pps_slice_chroma_qp_offsets_present_flag;
        pp.weighted_pred_flag      = p.weighted_pred_flag;
        pp.weighted_bipred_flag    = p.weighted_bipred_flag;
        pp.transquant_bypass_enabled_flag = p.transquant_bypass_enabled_flag;
        pp.tiles_enabled_flag      = p.tiles_enabled_flag;
        pp.entropy_coding_sync_enabled_flag = p.entropy_coding_sync_enabled_flag;
        pp.uniform_spacing_flag = 1;
        pp.loop_filter_across_tiles_enabled_flag = 0;
        pp.pps_loop_filter_across_slices_enabled_flag = p.pps_loop_filter_across_slices_enabled_flag;
        pp.deblocking_filter_override_enabled_flag = p.deblocking_filter_override_enabled_flag;
        pp.pps_deblocking_filter_disabled_flag = p.pps_deblocking_filter_disabled_flag;
        pp.lists_modification_present_flag = p.lists_modification_present_flag;
        pp.slice_segment_header_extension_present_flag = p.slice_segment_header_extension_present_flag;

        const bool irap = is_irap(nuh_type);
        const bool idr  = is_idr(nuh_type);
        pp.IrapPicFlag  = irap ? 1u : 0u;
        pp.IdrPicFlag   = idr  ? 1u : 0u;
        pp.IntraPicFlag = irap ? 1u : 0u;

        pp.pps_cb_qp_offset      = p.cb_qp_offset;
        pp.pps_cr_qp_offset      = p.cr_qp_offset;
        pp.num_tile_columns_minus1 = 0;
        pp.num_tile_rows_minus1    = 0;
        pp.diff_cu_qp_delta_depth  = p.diff_cu_qp_delta_depth;
        pp.pps_beta_offset_div2    = p.pps_beta_offset_div2;
        pp.pps_tc_offset_div2      = p.pps_tc_offset_div2;
        pp.log2_parallel_merge_level_minus2 = p.log2_parallel_merge_level_minus2;
        pp.CurrPicOrderCntVal = poc;

        // 默认全 invalid（IRAP / IDR 路径）
        for (int i = 0; i < 15; ++i) {
            pp.RefPicList[i].Index7Bits     = kInvalidIdx & 0x7Fu;
            pp.RefPicList[i].AssociatedFlag = 1;
            pp.PicOrderCntValList[i]        = 0;
        }
        for (int i = 0; i < 8; ++i) {
            pp.RefPicSetStCurrBefore[i] = kInvalidIdx;
            pp.RefPicSetStCurrAfter[i]  = kInvalidIdx;
            pp.RefPicSetLtCurr[i]       = kInvalidIdx;
        }
        // P-slice：简化 RPS — 单一 ref 指向上一张已解码图片。
        // 适用前提：低延时 IPC 流 IPP 模式，POC 单调步进（已在日志中验证）。
        // 不适用 B-frame；reorder>0 流应走 MFT 路径（已被 controller 排到此前）。
        if (!irap && last_ref_dpb_idx != UINT_MAX && last_ref_dpb_idx < dpb_size) {
            pp.RefPicList[0].Index7Bits     = static_cast<UCHAR>(last_ref_dpb_idx & 0x7Fu);
            pp.RefPicList[0].AssociatedFlag = 0;             // short-term，前向
            pp.PicOrderCntValList[0]        = last_ref_poc;
            pp.RefPicSetStCurrBefore[0]     = 0;             // 指 RefPicList[0]
        }
        pp.StatusReportFeedbackNumber = poc + 1;
    }

    bool ensure_output_pool() noexcept {
        return out_pool != nullptr;
    }

    uint32_t pic_count{0};
    uint32_t emit_count{0};

    // 提交 pic_params + slice_ctrl + bitstream → DecoderEndFrame → 拷贝到 output pool → emit
    bool flush_pending_pic() noexcept {
        if (!has_pic || !decoder || !cur_sps || !cur_pps) {
            has_pic = false;
            return false;
        }
        ++pic_count;
        const bool trace_first = pic_count <= 3;
        if (trace_first) {
            MCP_LOGF(pal::LogLevel::info,
                     "CodecDxvaVideo: flush#%u nal=%u poc=%d dpb=%u slices=%zu bs_bytes=%zu",
                     pic_count, cur_nal_type, cur_poc, cur_dpb_idx,
                     slice_ctrl.size(), bitstream_buf.size());
        }
        DXVA_PicParams_HEVC pp{};
        fill_pic_params(pp, *cur_sps, *cur_pps, cur_nal_type, cur_poc, cur_dpb_idx);

        HRESULT hr = video_ctx->DecoderBeginFrame(decoder.Get(),
                                                    dpb_views[cur_dpb_idx].Get(),
                                                    0, nullptr);
        if (FAILED(hr)) {
            MCP_LOGF(pal::LogLevel::warn,
                     "CodecDxvaVideo: DecoderBeginFrame hr=0x%08lX", hr);
            has_pic = false;
            return false;
        }
        if (trace_first) {
            MCP_LOGF(pal::LogLevel::info,
                     "CodecDxvaVideo: flush#%u DecoderBeginFrame ok", pic_count);
        }

        auto submit_buffer = [&](D3D11_VIDEO_DECODER_BUFFER_TYPE type,
                                  const void* src, std::size_t bytes) noexcept -> bool {
            UINT  buf_size = 0;
            void* buf_ptr  = nullptr;
            if (FAILED(video_ctx->GetDecoderBuffer(decoder.Get(), type, &buf_size, &buf_ptr))) return false;
            if (bytes > buf_size) {
                video_ctx->ReleaseDecoderBuffer(decoder.Get(), type);
                return false;
            }
            std::memcpy(buf_ptr, src, bytes);
            return SUCCEEDED(video_ctx->ReleaseDecoderBuffer(decoder.Get(), type));
        };

        const bool ok_pp = submit_buffer(D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS,
                                          &pp, sizeof(pp));
        const bool ok_sc = submit_buffer(D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL,
                                          slice_ctrl.data(),
                                          slice_ctrl.size() * sizeof(DXVA_Slice_HEVC_Short));
        const bool ok_bs = submit_buffer(D3D11_VIDEO_DECODER_BUFFER_BITSTREAM,
                                          bitstream_buf.data(), bitstream_buf.size());

        if (ok_pp && ok_sc && ok_bs) {
            D3D11_VIDEO_DECODER_BUFFER_DESC bd[3]{};
            bd[0].BufferType = D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS;
            bd[0].DataSize   = sizeof(pp);
            bd[1].BufferType = D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL;
            bd[1].DataSize   = static_cast<UINT>(slice_ctrl.size() * sizeof(DXVA_Slice_HEVC_Short));
            bd[2].BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM;
            bd[2].DataSize   = static_cast<UINT>(bitstream_buf.size());
            hr = video_ctx->SubmitDecoderBuffers(decoder.Get(), 3, bd);
            if (FAILED(hr)) {
                MCP_LOGF(pal::LogLevel::warn,
                         "CodecDxvaVideo: SubmitDecoderBuffers hr=0x%08lX", hr);
            }
        } else {
            MCP_LOGF(pal::LogLevel::warn,
                     "CodecDxvaVideo: GetDecoderBuffer/Release failed (pp=%d sc=%d bs=%d)",
                     ok_pp, ok_sc, ok_bs);
        }

        hr = video_ctx->DecoderEndFrame(decoder.Get());
        if (FAILED(hr)) {
            MCP_LOGF(pal::LogLevel::warn,
                     "CodecDxvaVideo: DecoderEndFrame hr=0x%08lX", hr);
        }
        if (trace_first) {
            MCP_LOGF(pal::LogLevel::info,
                     "CodecDxvaVideo: flush#%u DecoderEndFrame hr=0x%08lX", pic_count, hr);
        }

        // 标记 DPB 槽
        dpb[cur_dpb_idx].used = true;
        dpb[cur_dpb_idx].poc  = cur_poc;
        dpb[cur_dpb_idx].output_pending = true;

        // 拷贝 DPB slice → output pool slice（对 BIND_DECODER → BIND_SHADER_RESOURCE 跨 BindFlag 拷贝）。
        // ArraySize=1 的 fallback pool，dst slice 必须是 0；DPB slice 仍然按 cur_dpb_idx。
        const UINT out_slice = out_pool_array_capable ? out_next : 0;
        if (out_pool_array_capable) {
            out_next = (out_next + 1) % out_pool_size;
        }
        ctx->CopySubresourceRegion(out_pool.Get(), out_slice, 0, 0, 0,
                                    dpb_tex.Get(), cur_dpb_idx, nullptr);
        if (trace_first) {
            MCP_LOGF(pal::LogLevel::info,
                     "CodecDxvaVideo: flush#%u CopySubresourceRegion done dpb=%u→pool=%u",
                     pic_count, cur_dpb_idx, out_slice);
        }

        // emit
        VideoFrame f;
        f.pts_us           = cur_pts_us;
        f.arrival_qpc_ns   = cur_arrival_qpc_ns;
        f.width            = dec_w;
        f.height           = dec_h;
        f.source           = FrameSource::mft_dxva;
        f.dxva_texture     = out_pool;
        f.dxva_array_slice = out_slice;
        f.is_keyframe      = is_irap(cur_nal_type);
        f.color_primaries  = cur_sps->colour_desc_present ?
                              static_cast<mc_color_primaries_t>(cur_sps->colour_primaries) :
                              MC_COLOR_PRIMARIES_BT709;
        f.color_matrix     = cur_sps->colour_desc_present ?
                              static_cast<mc_color_matrix_t>(cur_sps->matrix_coeffs) :
                              MC_COLOR_MATRIX_BT709;
        f.color_range      = cur_sps->video_full_range ? MC_COLOR_RANGE_FULL : MC_COLOR_RANGE_LIMITED;
        // §5.13 Frame Validity Gate — 按真实状态描述六类 bit。
        // 第一张 IRAP 出来后 last_ref_dpb_idx 总有值，refs/recovery 同步满足。
        const bool irap_now      = is_irap(cur_nal_type);
        const bool anchor_armed  = irap_now || (last_ref_dpb_idx != UINT_MAX);
        uint32_t mask = kValidityParams | kValidityColor | kValidityReorder | kValidityFence;
        if (anchor_armed) mask |= kValidityRefs | kValidityRecovery;
        f.validity_mask = mask;
        if (emit_count == 0) {
            MCP_LOGF(pal::LogLevel::info,
                     "CodecDxvaVideo: first frame emit pts=%lld slice=%u poc=%d",
                     static_cast<long long>(cur_pts_us), out_slice, cur_poc);
        }
        if (cfg.emit) cfg.emit(std::move(f));
        ++emit_count;

        // 把当前帧记为下一张的唯一 short-term ref（IPP 链式）。
        last_ref_dpb_idx = cur_dpb_idx;
        last_ref_poc     = cur_poc;

        has_pic = false;
        bitstream_buf.clear();
        slice_ctrl.clear();
        return true;
    }

    // 主入口：单 AU 喂入。arrival_qpc_ns 端到端延时探针起点（first-packet RX 戳）。
    void on_au(std::span<const uint8_t> au, int64_t pts_us, int64_t arrival_qpc_ns = 0) noexcept {
        // find_start_code 返回的是「00 00 01」三元组的起点。
        // 4 字节 SC（00 00 00 01）找到时，返回的是第二个 0 的位置 (i)，前面 i-1 还有一个 0。
        // 不论 3 或 4 字节，NAL data 都从 i+3 起；start code 块从 i-(i 前是否多一个 0) 起。
        std::size_t i = find_start_code(au, 0);
        while (i < au.size()) {
            const bool        leading_zero = (i > 0 && au[i - 1] == 0);
            const std::size_t sc_start     = leading_zero ? i - 1 : i;
            const std::size_t nal_start    = i + 3;
            const std::size_t next         = find_start_code(au, nal_start);
            std::size_t       end          = next;
            if (next < au.size() && next > 0 && au[next - 1] == 0) --end;
            if (end <= nal_start + 2) { i = next; continue; }

            // 给 DXVA 的 bitstream 必须是带起始码的 Annex-B：从 sc_start 起；
            // RBSP 抽取从 NAL header 后两字节起。
            std::span<const uint8_t> nal_full = au.subspan(sc_start, end - sc_start);
            std::span<const uint8_t> ebsp     = au.subspan(nal_start + 2, end - nal_start - 2);

            const uint8_t b0 = au[nal_start];
            const uint8_t nuh_type = (b0 >> 1) & 0x3F;

            switch (nuh_type) {
                case kNalVpsNut: {
                    extract_rbsp(ebsp.data(), ebsp.size(), scratch_rbsp);
                    Vps v{};
                    if (parse_vps(scratch_rbsp, v)) vps[v.id] = v;
                    break;
                }
                case kNalSpsNut: {
                    extract_rbsp(ebsp.data(), ebsp.size(), scratch_rbsp);
                    Sps s{};
                    if (parse_sps(scratch_rbsp, s)) sps[s.id] = s;
                    break;
                }
                case kNalPpsNut: {
                    extract_rbsp(ebsp.data(), ebsp.size(), scratch_rbsp);
                    Pps p{};
                    if (parse_pps(scratch_rbsp, p)) pps[p.id] = p;
                    break;
                }
                case kNalAudNut:
                case kNalPrefixSei:
                case kNalSuffixSei:
                    break;
                default: {
                    if (!is_slice(nuh_type)) break;
                    if (sps.empty() || pps.empty()) break;

                    extract_rbsp(ebsp.data(), ebsp.size(), scratch_rbsp);
                    Pps& p = pps.begin()->second;
                    auto sit = sps.find(p.sps_id);
                    if (sit == sps.end()) break;
                    Sps& s = sit->second;
                    SliceHdr sh{};
                    if (!parse_slice_header_min(scratch_rbsp, nuh_type, s, p, sh)) break;

                    // 初始化 decoder（按首个有效 SPS）。
                    if (!decoder_inited) {
                        if (FAILED(init_decoder(s))) break;
                    }

                    // first slice → 启动新 picture
                    if (sh.first_slice_segment_in_pic_flag && !sh.dependent_slice_segment_flag) {
                        if (has_pic) flush_pending_pic();        // 上一张未冲（理论不该发生）
                        cur_sps      = &s;
                        cur_pps      = &p;
                        cur_nal_type = nuh_type;
                        cur_pts_us   = pts_us;
                        cur_arrival_qpc_ns = arrival_qpc_ns;
                        cur_poc      = compute_poc(nuh_type, sh.slice_pic_order_cnt_lsb,
                                                    s.log2_max_pic_order_cnt_lsb_minus4);
                        cur_dpb_idx  = alloc_dpb_slot();
                        bitstream_buf.clear();
                        slice_ctrl.clear();
                        has_pic = true;
                    }

                    if (!has_pic) break;     // 中间切片但首切片未到（异常）

                    // 累积 Annex-B 比特流（连同起始码一起）。
                    DXVA_Slice_HEVC_Short sc{};
                    sc.BSNALunitDataLocation = static_cast<UINT>(bitstream_buf.size());
                    sc.SliceBytesInBuffer    = static_cast<UINT>(nal_full.size());
                    sc.wBadSliceChopping     = 0;
                    bitstream_buf.insert(bitstream_buf.end(), nal_full.begin(), nal_full.end());
                    slice_ctrl.push_back(sc);
                    break;
                }
            }
            i = next;
        }

        // AU 末：刷该帧
        if (has_pic) {
            // bitstream 必须 128 字节对齐（DXVA 要求）。后补 0 直到对齐。
            const std::size_t align = 128;
            const std::size_t pad   = (align - (bitstream_buf.size() % align)) % align;
            bitstream_buf.insert(bitstream_buf.end(), pad, 0);
            flush_pending_pic();
        }
    }

    void reset_dpb() noexcept {
        for (auto& e : dpb) e = DpbEntry{};
        prev_poc_msb     = 0;
        prev_poc_lsb     = 0;
        last_ref_dpb_idx = UINT_MAX;
        last_ref_poc     = 0;
    }

    void release_all() noexcept {
        decoder.Reset();
        dpb_views.clear();
        dpb_tex.Reset();
        out_pool.Reset();
        video_ctx.Reset();
        video_device.Reset();
        ctx.Reset();
        decoder_inited = false;
    }
};

CodecDxvaVideo::CodecDxvaVideo(Config cfg) : impl_{std::make_unique<Impl>()} {
    impl_->cfg = std::move(cfg);
}
CodecDxvaVideo::~CodecDxvaVideo() { stop(); }

mc_status_t CodecDxvaVideo::start() noexcept {
    if (!impl_->cfg.device) return MC_ERR_INVALID_ARG;
    if (impl_->cfg.codec != MC_VIDEO_CODEC_H265) return MC_ERR_UNSUPPORTED;

    // multi-thread protect — 解码器跨线程
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(impl_->cfg.device.As(&mt))) mt->SetMultithreadProtected(TRUE);

    if (FAILED(impl_->init_video_device())) return MC_ERR_INTERNAL;

    // 探测 HEVC profile 是否被驱动支持。
    BOOL supported = FALSE;
    HRESULT hr = impl_->video_device->CheckVideoDecoderFormat(
        &impl_->profile, DXGI_FORMAT_NV12, &supported);
    if (FAILED(hr) || !supported) {
        MCP_LOGF(pal::LogLevel::error,
                 "CodecDxvaVideo: HEVC Main not supported by GPU driver hr=0x%08lX", hr);
        return MC_ERR_NO_HARDWARE;
    }
    MCP_LOGF(pal::LogLevel::info,
             "CodecDxvaVideo: GPU supports HEVC Main, awaiting SPS to size decoder");

    // 启动 T4-DXVA worker：submit 不再同步阻塞 RX 线程。
    impl_->worker_stop.store(false, std::memory_order_release);
    impl_->worker_thread = std::thread([impl = impl_.get()] { impl->worker_loop(); });
    return MC_OK;
}

void CodecDxvaVideo::submit(std::vector<uint8_t> au_bytes, int64_t pts_us,
                              int64_t arrival_qpc_ns) noexcept {
    if (!impl_->video_device) return;
    Impl::PendingAu p;
    p.bytes          = std::move(au_bytes);
    p.pts_us         = pts_us;
    p.arrival_qpc_ns = arrival_qpc_ns;

    // SPSC try_push lock-free。满时 producer 不能动 tail（违反 SPSC 不变量），
    // 直接丢自己；consumer 在 try_pop 前 drop_to_last 维持"最新优先"。
    if (!impl_->au_queue.try_push(std::move(p))) {
        static std::atomic<uint64_t> dropped{0};
        const auto n = dropped.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1 || (n & (n - 1)) == 0) {
            MCP_LOGF(pal::LogLevel::warn,
                     "CodecDxvaVideo: au_queue full (cap=%zu), drop newest AU (#%llu)",
                     Impl::kAuQueueCap, static_cast<unsigned long long>(n));
        }
        return;
    }
    {
        std::scoped_lock lk{impl_->au_mu};   // happens-before barrier
    }
    impl_->au_cv.notify_one();
}

void CodecDxvaVideo::flush() noexcept {
    // SPSC 不变量：try_drop_oldest 是 consumer 操作，与 worker 端的 try_pop 串行。
    // 持 au_mu 排他（worker 也在该锁内做 SPSC pop）。
    {
        std::scoped_lock lk{impl_->au_mu};
        while (impl_->au_queue.try_drop_oldest()) { /* drain */ }
    }
    if (impl_->has_pic) impl_->flush_pending_pic();
    impl_->reset_dpb();
}

void CodecDxvaVideo::stop() noexcept {
    if (!impl_) return;
    // 阶段 1：通知 worker 退出并等其结束；之后 release D3D 资源才安全。
    impl_->worker_stop.store(true, std::memory_order_release);
    impl_->au_cv.notify_all();
    if (impl_->worker_thread.joinable()) impl_->worker_thread.join();

    flush();
    impl_->release_all();
}

void CodecDxvaVideo::Impl::worker_loop() noexcept {
    pal::ThreadRegistration reg;
    pal::ThreadOptions opt;
    opt.name        = "mc-player T4 DXVA-Decode";
    opt.mmcss_task  = pal::MmcssTask::playback;
    reg.apply(opt);

    while (!worker_stop.load(std::memory_order_acquire)) {
        PendingAu au;
        bool got = false;
        {
            std::unique_lock lk{au_mu};
            au_cv.wait(lk, [&]{
                return worker_stop.load(std::memory_order_acquire) ||
                        au_queue.approx_size() > 0;
            });
            if (worker_stop.load(std::memory_order_acquire)) break;
            // SPSC consumer 操作必须持 mu（与 flush() 互斥）；producer try_push 仍 lock-free。
            // "最新优先"：drop 到只剩 1 再 pop（兑现 CLAUDE.md "满时丢最老" 语义）。
            while (au_queue.approx_size() > 1) (void)au_queue.try_drop_oldest();
            got = au_queue.try_pop(au);
        }
        if (!got) continue;     // race / spurious wake，回 wait
        on_au(std::span<const uint8_t>(au.bytes.data(), au.bytes.size()),
              au.pts_us, au.arrival_qpc_ns);
    }
}

}  // namespace mcp::media
