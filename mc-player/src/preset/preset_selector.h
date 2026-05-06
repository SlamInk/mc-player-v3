/*
 * Preset Selector — 按 CapabilitySnapshot 选择 5 档 preset(capability_probe §6.2)。
 *
 * 匹配规则(优先级降序;命中即返,后续规则不评估):
 *
 *   SDI_REPLACEMENT:
 *     hw.vendor_sdk_present                          (任一 vendor SDK 在场)
 *     net.link_kind == LAN_SWITCHED && loss<0.001 && rtt<5ms
 *     enc.reorder_depth == 0                         (zerolatency 编码)
 *     render.refresh_rate_hz >= 144 && render.vrr_supported && render.supports_dcomp_nv12_direct (9.1)
 *
 *   REALTIME_LAN:
 *     hw.h264_supported || hw.hevc_main_supported    (任 codec 可硬解)
 *     net.link_kind == LAN_SWITCHED || LAN_WIFI
 *     net.rtt_p95_ms < 20 && net.loss_rate_short < 0.01
 *
 *   STREAMING_WIFI:
 *     net.link_kind == LAN_WIFI
 *     net.loss_rate_short < 0.03
 *
 *   WAN_FALLBACK:
 *     net.loss_rate_short >= 0.03 || rtt_p95_ms >= 100
 *
 *   SAFE_MODE:
 *     兜底(probe 不完整 / 全部条件未命中)
 */

#ifndef MC_PLAYER_PRESET_SELECTOR_H_
#define MC_PLAYER_PRESET_SELECTOR_H_

#include "mc-player/mc_player_stats.h"
#include "mc-player/mc_player_types.h"
#include "probe/capability_snapshot.h"

namespace mcp::preset {

mc_preset_id_t select_preset(const probe::CapabilitySnapshot& snap) noexcept;

}  // namespace mcp::preset

#endif  // MC_PLAYER_PRESET_SELECTOR_H_
