#include "media/color_meta.h"

namespace mcp::media {

namespace {

constexpr uint32_t kHeightHd    = 720;
constexpr uint32_t kHeightUhd   = 2160;

// ITU-T H.273 transfer_characteristics (相同编码与 H.264/H.265 SPS VUI):
//   16 = SMPTE ST 2084 (PQ)
//   18 = ARIB STD-B67 (HLG)
constexpr int kTransferPq  = 16;
constexpr int kTransferHlg = 18;

bool vui_self_consistent(const VuiInputs& v) noexcept {
    if (v.colour_primaries < 0 || v.matrix_coefficients < 0) return false;
    // 全 0 / 全 1 视为不可信（IPC 常见错误）。
    if (v.colour_primaries == 0 && v.matrix_coefficients == 0) return false;
    if (v.colour_primaries == 255 || v.matrix_coefficients == 255) return false;

    // 已知组合命中（ADD §5.12 表）
    const bool bt709  = (v.colour_primaries == 1 && v.matrix_coefficients == 1);
    const bool bt601  = (v.colour_primaries == 5 || v.colour_primaries == 6) &&
                         v.matrix_coefficients == 6;
    const bool bt2020 = (v.colour_primaries == 9 && v.matrix_coefficients == 9);
    return bt709 || bt601 || bt2020;
}

ResolvedColor pick_from_vui(const VuiInputs& v) noexcept {
    ResolvedColor r{};
    r.range = v.video_full_range_flag ? MC_COLOR_RANGE_FULL : MC_COLOR_RANGE_LIMITED;
    if (v.colour_primaries == 1 && v.matrix_coefficients == 1) {
        r.primaries = MC_COLOR_PRIMARIES_BT709;
        r.matrix    = MC_COLOR_MATRIX_BT709;
    } else if (v.colour_primaries == 5 || v.colour_primaries == 6) {
        r.primaries = MC_COLOR_PRIMARIES_BT601;
        r.matrix    = MC_COLOR_MATRIX_BT601;
    } else if (v.colour_primaries == 9) {
        r.primaries = MC_COLOR_PRIMARIES_BT2020;
        r.matrix    = MC_COLOR_MATRIX_BT2020_NCL;
    } else {
        r.primaries = MC_COLOR_PRIMARIES_BT709;
        r.matrix    = MC_COLOR_MATRIX_BT709;
    }
    return r;
}

ResolvedColor pick_heuristic(const VuiInputs& v, uint32_t height_px) noexcept {
    ResolvedColor r{};
    r.range = MC_COLOR_RANGE_LIMITED;       // SD / HD 默认 limited

    const bool hdr = (v.transfer_characteristics == kTransferPq) ||
                     (v.transfer_characteristics == kTransferHlg);
    if (height_px >= kHeightUhd && hdr) {
        r.primaries = MC_COLOR_PRIMARIES_BT2020;
        r.matrix    = MC_COLOR_MATRIX_BT2020_NCL;
    } else if (height_px >= kHeightHd) {
        r.primaries = MC_COLOR_PRIMARIES_BT709;
        r.matrix    = MC_COLOR_MATRIX_BT709;
    } else {
        r.primaries = MC_COLOR_PRIMARIES_BT601;
        r.matrix    = MC_COLOR_MATRIX_BT601;
    }
    return r;
}

}  // namespace

ResolvedColor resolve_color(const VuiInputs& vui,
                             uint32_t height_px,
                             const ColorOverride& user_override) noexcept {
    // 优先级：用户覆盖 > VUI 自洽 > 启发式
    ResolvedColor base = vui_self_consistent(vui) ? pick_from_vui(vui)
                                                  : pick_heuristic(vui, height_px);

    if (user_override.primaries != MC_COLOR_PRIMARIES_AUTO) base.primaries = user_override.primaries;
    if (user_override.range     != MC_COLOR_RANGE_AUTO)     base.range     = user_override.range;
    if (user_override.matrix    != MC_COLOR_MATRIX_AUTO)    base.matrix    = user_override.matrix;

    return base;
}

}  // namespace mcp::media
