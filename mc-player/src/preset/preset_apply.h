/*
 * Preset Apply — 一次性 apply preset 到 6 个子系统(decoder / jitter / render / present / RTCP / gate)。
 *
 * 实装边界(Phase 9.0 结构性骨架):
 *   - apply 接口骨架,各子系统调 apply_preset(preset);幂等
 *   - graceful degrade(capability_probe §7.1):任一子系统未实装 → 退到次档配置 +
 *     mc.preset.apply_partial_count 上报,不整体回滚
 *   - 整体 SAFE_MODE 回滚仅作兜底,warm_steady = 0 永久
 */

#ifndef MC_PLAYER_PRESET_APPLY_H_
#define MC_PLAYER_PRESET_APPLY_H_

#include "mc-player/mc_player_stats.h"
#include "mc-player/mc_player_types.h"
#include "preset/preset_definitions.h"

namespace mcp::preset {

/// 一次性 apply 5 档 preset 到 6 个子系统。
mc_status_t apply_preset(const Preset& preset) noexcept;

/// 上报子系统 graceful degrade(capability_probe §7.1)。
void emit_apply_partial(const char* subsystem, const char* target_tier,
                          const char* actual_tier) noexcept;

}  // namespace mcp::preset

#endif  // MC_PLAYER_PRESET_APPLY_H_
