/*
 * AAC Depacketizer — RFC 3640 mpeg4-generic AAC-hbr。
 *
 * AU header 由 SDP fmtp 的 sizeLength / indexLength 等参数描述；
 * AudioSpecificConfig 来自 fmtp config=（hex 编码）。
 *
 * 输出 raw AAC frame（含原始 ADTS-less payload）+ AudioSpecificConfig；
 * 喂给 Microsoft AAC MFT 时用 payload type 0 (Raw)。
 *
 * 不支持 LATM/LOAS over RTP（RFC 6416 mode=AAC-LATM）— 后续按需追加。
 */

#ifndef MC_PLAYER_MEDIA_DEPACK_AAC_H_
#define MC_PLAYER_MEDIA_DEPACK_AAC_H_

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace mcp::media {

struct AacAccessUnit {
    int64_t                 pts_us         = 0;
    std::vector<uint8_t>    raw_aac;
};

class DepackAac {
public:
    using EmitFn = std::function<void(AacAccessUnit&&)>;
    explicit DepackAac(EmitFn emit) noexcept;

    void set_audio_specific_config_hex(std::string_view hex) noexcept;
    void set_au_header_lengths(uint32_t size_length, uint32_t index_length) noexcept;

    [[nodiscard]] std::span<const uint8_t> audio_specific_config() const noexcept {
        return {audio_specific_config_};
    }

    void on_rtp(int64_t pts_us, std::span<const uint8_t> payload) noexcept;
    void reset() noexcept;

private:
    EmitFn                  emit_;
    std::vector<uint8_t>    audio_specific_config_;
    uint32_t                size_length_  = 13;       // RFC 3640 默认
    uint32_t                index_length_ = 3;
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_DEPACK_AAC_H_
