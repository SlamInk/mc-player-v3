/*
 * Render Probe — 显示器刷新率 / VRR / DCOMP / MPO planes 探测(capability_probe §3.5)。
 *
 * 实装边界(Phase 9.0):
 *   - refresh_rate_hz / vrr_supported / dcomp_supported / mpo_planes 探测落地
 *   - supports_dcomp_nv12_direct 默认 false,Phase 9.1 才 unlock
 */

#ifndef MC_PLAYER_PROBE_RENDER_PROBE_H_
#define MC_PLAYER_PROBE_RENDER_PROBE_H_

#include <Windows.h>

#include "probe/capability_snapshot.h"

namespace mcp::probe {

/// 同步 + 本地查询,p95 < 50ms(plan §9.0.3)。
RenderSnapshot run_render_probe(HWND hwnd) noexcept;

}  // namespace mcp::probe

#endif  // MC_PLAYER_PROBE_RENDER_PROBE_H_
