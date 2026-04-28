/*
 * H.264 Depacketizer — RFC 6184。
 *
 * 支持的 NAL 类型（packetization-mode = 1，non-interleaved）：
 *   - 单 NAL（type 1-23）：直接发出
 *   - STAP-A（24）：拆解为多个单 NAL
 *   - FU-A（28）：跨 RTP 包重组成单 NAL
 *
 * 拒绝 packetization-mode=2（STAP-B/MTAP/FU-B：25/26/27/29）。SDP answer 阶段拒。
 *
 * 输出：完整 access unit（含 SPS/PPS/IDR/Slice/SEI），带 Annex-B start code (00 00 00 01)。
 *
 * Refresh anchor 标记：
 *   - IDR (NAL=5) → recovery_complete = true
 *   - SEI (NAL=6) 内含 recovery_point 且 recovery_frame_cnt == 0 → recovery_complete = true
 *
 * 参考帧丢失：caller 在 jitter buffer 报 RTP gap 时调 mark_reference_lost()，
 * 后续所有帧标记 invalid 直至下一 refresh anchor。
 */

#ifndef MC_PLAYER_MEDIA_DEPACK_H264_H_
#define MC_PLAYER_MEDIA_DEPACK_H264_H_

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace mcp::media {

struct H264AccessUnit {
    int64_t                  pts_us           = 0;
    std::vector<uint8_t>     annexb_bytes;          // 含 start code
    bool                     has_idr          = false;
    bool                     has_recovery_sei = false;     // recovery_frame_cnt == 0
    bool                     refs_lost        = false;     // 仍处于 invalid 段
    bool                     params_present   = false;     // SPS/PPS 已缓存
};

class DepackH264 {
public:
    using EmitFn = std::function<void(H264AccessUnit&&)>;

    explicit DepackH264(EmitFn emit) noexcept;

    /// 设置 SDP 提供的 sprop-parameter-sets（base64 编码 SPS,PPS 列表，逗号分隔）。
    /// 缓存为 extradata；首帧解码前置入 access unit 头，避免等带内 SPS。
    void set_sprop_parameter_sets(std::string_view base64_csv) noexcept;

    /// 入参：单条 RTP payload + 元信息。
    void on_rtp(int64_t pts_us, bool marker, std::span<const uint8_t> payload) noexcept;

    /// jitter buffer 报告 RTP 序号 gap → 标记后续所有帧 invalid 直至 anchor。
    void mark_reference_lost() noexcept;

    /// Device Lost / 重连 → 重置 FU 重组状态、恢复标记、缓存 SPS/PPS。
    void reset() noexcept;

private:
    void emit_au(int64_t pts_us, bool with_extradata) noexcept;

    EmitFn               emit_;
    std::vector<uint8_t> sps_;
    std::vector<uint8_t> pps_;
    std::vector<uint8_t> au_buffer_;       // 当前正在拼装的 AU（Annex-B）
    std::vector<uint8_t> fu_buffer_;       // FU-A 重组中的单 NAL
    bool                 fu_in_progress_     = false;
    uint8_t              fu_nal_header_byte_ = 0;
    bool                 refs_lost_          = true;        // 启动期默认 invalid，等首个 anchor
    bool                 saw_idr_in_au_      = false;
    bool                 saw_recovery_in_au_ = false;
    int64_t              current_pts_us_     = 0;
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_DEPACK_H264_H_
