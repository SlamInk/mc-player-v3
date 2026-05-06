/*
 * HDCM Detector — 启动期 batch detect 7 类组件状态(plan §8.0 / hdcm 设计 §5.1)。
 *
 * 实装边界(Phase 8-A):
 *   - 类别 A: VendorId + DLL 存在性双查(LoadLibraryW + %LOCALAPPDATA% 缓存)
 *   - 类别 B/C/D: 8-B/8-C/8-D 各自实装,本 phase 默认 unknown
 */

#ifndef MC_PLAYER_HDCM_DETECTOR_H_
#define MC_PLAYER_HDCM_DETECTOR_H_

#include <cstdint>
#include <vector>

#include "hdcm/manifest_table.h"

namespace mcp::hdcm {

struct ComponentSnapshot {
    const ComponentManifest* manifest = nullptr;
    State                    state    = State::unknown;
    uint32_t                 detected_vendor_id = 0;     // 仅类别 A/D
    std::string              installed_version;          // 类别 A：DLL 路径 / 版本(Phase 8-A 暂留空)
};

/// 异步运行,不阻塞 mc_open；结果在 metric 与回调中暴露。
std::vector<ComponentSnapshot> detect_all_components(uint32_t adapter_vendor_id) noexcept;

/// 单组件探测(供 ui_panel "刷新"按钮)。
ComponentSnapshot detect_component(const ComponentManifest& m, uint32_t adapter_vendor_id) noexcept;

}  // namespace mcp::hdcm

#endif  // MC_PLAYER_HDCM_DETECTOR_H_
