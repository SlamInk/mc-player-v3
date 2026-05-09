/*
 * HDCM (Hardware Decode Component Manager) Manifest Table —
 *   ADR-016 + ADR-021 静态组件表(7 类组件 × 4 类别)。
 *
 * 类别(详 docs/mc-player_hdcm_设计.md §2):
 *   A. Vendor SDK              — A_NVDEC / A_oneVPL / A_AMF
 *   B. Microsoft Store 媒体扩展 — B_HEVC_Ext / B_AV1_Ext
 *   C. Windows Optional Feature — C_MediaPlayback (启用 MFT registry)
 *   D. GPU driver 阈值          — D_NV_Driver / D_Intel_Driver / D_AMD_Driver
 *
 * 实装边界(Phase 8-A): 仅类别 A 三组件 manifest 完整；B/C/D 在后续 sub-phase 接入。
 */

#ifndef MC_PLAYER_HDCM_MANIFEST_TABLE_H_
#define MC_PLAYER_HDCM_MANIFEST_TABLE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace mcp::hdcm {

enum class Category : uint8_t {
    A_VendorSdk      = 1,
    B_StoreExtension = 2,
    C_OptionalFeature = 3,
    D_GpuDriver      = 4,
};

enum class State : uint8_t {
    unknown                 = 0,
    already_installed       = 1,
    installable             = 2,
    installing              = 3,
    install_failed          = 4,
    restart_pending         = 5,
    unavailable_on_this_sku = 6,
    user_skipped            = 7,
};

struct ComponentManifest {
    const char* id              = "";
    Category    category        = Category::A_VendorSdk;
    uint32_t    target_vendor_id = 0;     // 0 = 与 vendor 无关(类别 B/C)
    const char* dll_name        = "";     // 类别 A：探测 DLL；类别 B/C：空
    const char* download_url    = "";     // 类别 A：HTTPS URL；类别 D：vendor 官网
    const char* sha256          = "";     // 类别 A 必有；类别 B/C/D 空
    const char* package_family  = "";     // 类别 B：PackageFamilyName
    const char* feature_name    = "";     // 类别 C：DISM feature name
    uint32_t    driver_min_major = 0;     // 类别 D：驱动版本阈值四段
    uint32_t    driver_min_minor = 0;
    uint32_t    driver_min_build = 0;
    uint32_t    driver_min_revision = 0;
};

/// 编译期表(plan §8.1 + hdcm 设计 §2)。
const std::vector<ComponentManifest>& manifest_table() noexcept;

const char* category_label(Category c) noexcept;
const char* state_label(State s) noexcept;

}  // namespace mcp::hdcm

#endif  // MC_PLAYER_HDCM_MANIFEST_TABLE_H_
