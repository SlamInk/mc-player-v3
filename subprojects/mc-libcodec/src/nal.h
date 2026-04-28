/*
 * NAL 切分 + RBSP 提取（ITU-T H.265 §7.3.1.1 / §B.2）。
 *
 * 输入：Annex-B 字节流（可能含多个 NAL，每段以 0x000001 / 0x00000001 起始）。
 * 输出：逐个 NAL 的 nuh + RBSP 字节（已剔 emulation_prevention_three_byte 0x03）。
 */

#ifndef MC_LIBCODEC_NAL_H_
#define MC_LIBCODEC_NAL_H_

#include <cstdint>
#include <cstddef>
#include <functional>
#include <span>
#include <vector>

namespace mclc {

/// H.265 NAL header（2 字节）。
struct HevcNalHeader {
    uint8_t  forbidden_zero_bit;
    uint8_t  nal_unit_type;       // 6 bit：0-63
    uint8_t  layer_id;            // 6 bit
    uint8_t  temporal_id_plus1;   // 3 bit；TID = -1
};

/// Annex-B 起始码扫描；逐 NAL 回调 (header, rbsp_bytes)。
/// rbsp_bytes 已剔除 emulation_prevention_three_byte。回调中 span 仅在回调内有效。
using NalCallback = std::function<void(const HevcNalHeader&, std::span<const uint8_t> rbsp)>;

void hevc_split_nals(std::span<const uint8_t> annexb,
                      const NalCallback&        cb,
                      std::vector<uint8_t>&     scratch_rbsp) noexcept;

/// NAL type 范围（§7.4.2.2）。
constexpr uint8_t kNalTrailN     = 0;
constexpr uint8_t kNalTrailR     = 1;
constexpr uint8_t kNalIdrWRadl   = 19;
constexpr uint8_t kNalIdrNLp     = 20;
constexpr uint8_t kNalCraNut     = 21;
constexpr uint8_t kNalRaslN      = 8;
constexpr uint8_t kNalRaslR      = 9;
constexpr uint8_t kNalRadlN      = 6;
constexpr uint8_t kNalRadlR      = 7;
constexpr uint8_t kNalVpsNut     = 32;
constexpr uint8_t kNalSpsNut     = 33;
constexpr uint8_t kNalPpsNut     = 34;
constexpr uint8_t kNalAudNut     = 35;
constexpr uint8_t kNalEosNut     = 36;
constexpr uint8_t kNalEobNut     = 37;
constexpr uint8_t kNalFdNut      = 38;
constexpr uint8_t kNalPrefixSei  = 39;
constexpr uint8_t kNalSuffixSei  = 40;

[[nodiscard]] constexpr bool is_irap(uint8_t t) noexcept { return t >= 16 && t <= 23; }
[[nodiscard]] constexpr bool is_idr(uint8_t t)  noexcept { return t == kNalIdrWRadl || t == kNalIdrNLp; }
[[nodiscard]] constexpr bool is_slice(uint8_t t) noexcept { return t <= 31; }

}  // namespace mclc

#endif  // MC_LIBCODEC_NAL_H_
