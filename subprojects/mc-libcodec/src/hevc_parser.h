/*
 * HEVC parameter set / slice header parser（ITU-T H.265 §7.3.2 / §7.4.2）。
 *
 * 输入：RBSP 字节序列（已剔 emulation prevention）。
 * 输出：填充对应 syntax struct。失败返 false。
 *
 * 仅 Main profile 4:2:0 8-bit；scaling list / HRD / 3D 扩展跳过解析。
 */

#ifndef MC_LIBCODEC_HEVC_PARSER_H_
#define MC_LIBCODEC_HEVC_PARSER_H_

#include <cstdint>
#include <span>

#include "hevc_syntax.h"

namespace mclc {

bool parse_vps(std::span<const uint8_t> rbsp, HevcVps& out) noexcept;
bool parse_sps(std::span<const uint8_t> rbsp, HevcSps& out) noexcept;
bool parse_pps(std::span<const uint8_t> rbsp, HevcPps& out, const HevcSps& sps) noexcept;

/// 仅解析 slice segment header（不含 slice data）。需要对应的 PPS / SPS。
/// nuh_type 用于 IDR / IRAP 判定（§7.4.7.1）。
bool parse_slice_header(std::span<const uint8_t> rbsp,
                         uint8_t                   nuh_type,
                         const HevcSps&            sps,
                         const HevcPps&            pps,
                         HevcSliceHeader&          out) noexcept;

}  // namespace mclc

#endif  // MC_LIBCODEC_HEVC_PARSER_H_
