/*
 * Encoder Probe — SDP profile-level-id + SPS VUI + 首 GOP 实测三档可信度(ADR-019)。
 */

#ifndef MC_PLAYER_PROBE_ENCODER_PROBE_H_
#define MC_PLAYER_PROBE_ENCODER_PROBE_H_

#include <cstdint>
#include <memory>
#include <string_view>

#include "probe/capability_snapshot.h"

namespace mcp::probe {

class EncoderProbe {
public:
    EncoderProbe();
    ~EncoderProbe();

    EncoderProbe(const EncoderProbe&)            = delete;
    EncoderProbe& operator=(const EncoderProbe&) = delete;

    /// SDP answer 解析阶段调一次。
    void record_sdp_profile_level_id(std::string_view profile_level_id) noexcept;

    /// 收到首个 SPS NAL 调一次。
    void record_sps_vui(uint32_t max_num_reorder_frames, bool bitstream_restriction_known) noexcept;

    /// 首 GOP DTS/PTS diff 实测兜底。
    void record_first_gop_dts_pts_diff_ms(uint32_t diff_ms) noexcept;

    [[nodiscard]] EncoderSnapshot snapshot() const noexcept;
    [[nodiscard]] bool ready() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp::probe

#endif  // MC_PLAYER_PROBE_ENCODER_PROBE_H_
