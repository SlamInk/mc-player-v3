/*
 * H.265 Depacketizer — RFC 7798。
 *
 * NAL header 是 2 字节（F(1) + Type(6) + LayerId(6) + TID(3) = 16 bits）。
 * FU 重组与 H.264 区别：FU 携带 1B FU header，重组时前置 2B 重建 NAL header；
 * 必须保留 LayerId/TID，按 FU header 内的 type 字段重组 Type 域。
 *
 * NAL Type 归属（ITU-T H.265 Table 7-1）：
 *   0-40: ITU-T 定义；41-47: 保留；48-63: unspecified。
 *   RFC 7798 占用 48 (AP) / 49 (FU) / 50 (PACI)。本模块不支持 50。
 *
 * Refresh anchor：IDR_W_RADL(19) / IDR_N_LP(20) / CRA_NUT(21) / BLA_W_LP(16) / BLA_W_RADL(17) / BLA_N_LP(18)
 * 视为 IRAP；recovery_point SEI(prefix 39) 含 recovery_frame_cnt==0 也视为 anchor。
 */

#ifndef MC_PLAYER_MEDIA_DEPACK_H265_H_
#define MC_PLAYER_MEDIA_DEPACK_H265_H_

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace mcp::media {

struct H265AccessUnit {
    int64_t              pts_us           = 0;
    int64_t              arrival_qpc_ns   = 0;              // first-packet RX 戳；端到端延时探针
    std::vector<uint8_t> annexb_bytes;
    bool                 has_irap         = false;
    bool                 has_recovery_sei = false;
    bool                 refs_lost        = false;
    bool                 params_present   = false;
};

/// SPS 解析信息（ADD §5.6.4 / §5.12）。
/// sps_max_num_reorder_pics 取最高 sub-layer 的值——0 即无 B 帧重排序。
struct H265SpsInfo {
    bool     parsed                    = false;
    uint32_t pic_width                 = 0;
    uint32_t pic_height                = 0;
    uint32_t max_num_reorder_pics      = 0;
    bool     vui_present               = false;
    int      colour_primaries          = -1;
    int      matrix_coefficients       = -1;
    int      transfer_characteristics  = -1;
    bool     video_full_range_flag     = false;
};

class DepackH265 {
public:
    using EmitFn = std::function<void(H265AccessUnit&&)>;
    explicit DepackH265(EmitFn emit) noexcept;

    /// SDP fmtp sprop-vps / sprop-sps / sprop-pps 任一三个分别注入（base64）。
    void set_sprop_vps(std::string_view base64) noexcept;
    void set_sprop_sps(std::string_view base64) noexcept;
    void set_sprop_pps(std::string_view base64) noexcept;

    void on_rtp(int64_t pts_us, bool marker, std::span<const uint8_t> payload,
                int64_t arrival_qpc_ns = 0) noexcept;
    void mark_reference_lost() noexcept;
    void reset() noexcept;

    const H265SpsInfo& sps_info() const noexcept { return sps_info_; }

private:
    void emit_au(int64_t pts_us, bool with_extradata) noexcept;
    void parse_sps_locked() noexcept;

    EmitFn               emit_;
    std::vector<uint8_t> vps_, sps_, pps_;
    H265SpsInfo          sps_info_;
    std::vector<uint8_t> au_buffer_;
    std::vector<uint8_t> fu_buffer_;
    bool                 fu_in_progress_     = false;
    bool                 saw_irap_in_au_     = false;
    bool                 saw_recovery_in_au_ = false;
    bool                 refs_lost_          = true;
    int64_t              current_pts_us_     = 0;
    int64_t              current_arrival_qpc_ns_ = 0;       // AU 起始包的 arrival 戳
    uint8_t              fu_layer_id_        = 0;
    uint8_t              fu_tid_             = 0;
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_DEPACK_H265_H_
