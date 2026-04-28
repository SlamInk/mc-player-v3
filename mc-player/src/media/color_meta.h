/*
 * 色彩元数据三级兜底（ADD §5.12）。
 *
 * 现网 IPC 大量出现 SPS VUI 缺失或填错；单纯按 VUI 取值会出现明显色偏。
 *
 * 三级：
 *   1. SPS VUI 自洽 → 取用
 *   2. VUI 不可信 → 启发式（按高度+transfer 推断 BT.601 / BT.709 / BT.2020）
 *   3. 用户显式覆盖（API 字段 / UI 设置）
 *
 * 锁定的最终值会置位 Frame Validity Gate 的 color_meta_known bit。
 */

#ifndef MC_PLAYER_MEDIA_COLOR_META_H_
#define MC_PLAYER_MEDIA_COLOR_META_H_

#include <cstdint>
#include <optional>

#include "mc-player/mc_player_types.h"

namespace mcp::media {

struct VuiInputs {
    int  colour_primaries        = -1;     // -1 = 未知 / 缺失
    int  matrix_coefficients     = -1;
    int  transfer_characteristics= -1;
    bool video_full_range_flag   = false;
};

struct ColorOverride {
    mc_color_primaries_t primaries = MC_COLOR_PRIMARIES_AUTO;
    mc_color_range_t     range     = MC_COLOR_RANGE_AUTO;
    mc_color_matrix_t    matrix    = MC_COLOR_MATRIX_AUTO;
};

struct ResolvedColor {
    mc_color_primaries_t primaries;
    mc_color_range_t     range;
    mc_color_matrix_t    matrix;
};

/// 三级兜底解析；成功必定返回值（启发式总能给出默认）。
ResolvedColor resolve_color(const VuiInputs& vui,
                             uint32_t height_px,
                             const ColorOverride& user_override) noexcept;

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_COLOR_META_H_
