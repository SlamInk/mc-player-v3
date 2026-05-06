/*
 * Hardware Probe — 现有 dxgi_caps_probe / vendor SDK probe 整合到统一接口(ADR-017)。
 */

#ifndef MC_PLAYER_PROBE_HARDWARE_PROBE_H_
#define MC_PLAYER_PROBE_HARDWARE_PROBE_H_

#include <Windows.h>

#include "probe/capability_snapshot.h"

namespace mcp::probe {

/// 同步执行,p95 < 200ms(plan §9.0.3)。
HardwareSnapshot run_hardware_probe(HWND hwnd) noexcept;

}  // namespace mcp::probe

#endif  // MC_PLAYER_PROBE_HARDWARE_PROBE_H_
