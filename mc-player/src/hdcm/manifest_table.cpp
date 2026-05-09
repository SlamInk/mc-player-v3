#include "hdcm/manifest_table.h"

namespace mcp::hdcm {

const std::vector<ComponentManifest>& manifest_table() noexcept {
    // 7 类组件静态表(详 docs/mc-player_hdcm_设计.md §2)。
    // download_url / sha256 留空: Phase 8-A 实装期再按 vendor 实际发布版本填入并入 ADR。
    static const std::vector<ComponentManifest> kTable = {
        // ─── 类别 A: Vendor SDK 直驱 ────────────────────────────
        { "A_NVDEC",   Category::A_VendorSdk, 0x10DE, "nvcuvid.dll", "", "", "", "", 0,0,0,0 },
        { "A_oneVPL",  Category::A_VendorSdk, 0x8086, "vpl.dll",     "", "", "", "", 0,0,0,0 },
        { "A_AMF",     Category::A_VendorSdk, 0x1002, "amfrt64.dll", "", "", "", "", 0,0,0,0 },

        // ─── 类别 B: Microsoft Store 媒体扩展 ───────────────────
        { "B_HEVC_Ext", Category::B_StoreExtension, 0, "", "", "",
          "Microsoft.HEVCVideoExtension_8wekyb3d8bbwe", "", 0,0,0,0 },
        { "B_AV1_Ext",  Category::B_StoreExtension, 0, "", "", "",
          "Microsoft.AV1VideoExtension_8wekyb3d8bbwe",  "", 0,0,0,0 },

        // ─── 类别 C: Windows Optional Feature ──────────────────
        { "C_MediaPlayback", Category::C_OptionalFeature, 0, "", "", "", "",
          "MediaPlayback", 0,0,0,0 },

        // ─── 类别 D: GPU driver 阈值(版本数字 Phase 8-D 实装期填入) ────
        { "D_NV_Driver",    Category::D_GpuDriver, 0x10DE, "", "https://www.nvidia.com/Download/index.aspx",
          "", "", "", 0,0,0,0 },
        { "D_Intel_Driver", Category::D_GpuDriver, 0x8086, "", "https://www.intel.com/content/www/us/en/download-center/home.html",
          "", "", "", 0,0,0,0 },
        { "D_AMD_Driver",   Category::D_GpuDriver, 0x1002, "", "https://www.amd.com/en/support",
          "", "", "", 0,0,0,0 },
    };
    return kTable;
}

const char* category_label(Category c) noexcept {
    switch (c) {
        case Category::A_VendorSdk:        return "A_vendor_sdk";
        case Category::B_StoreExtension:   return "B_store_ext";
        case Category::C_OptionalFeature:  return "C_optional_feature";
        case Category::D_GpuDriver:        return "D_gpu_driver";
    }
    return "unknown";
}

const char* state_label(State s) noexcept {
    switch (s) {
        case State::unknown:                  return "unknown";
        case State::already_installed:        return "already_installed";
        case State::installable:              return "installable";
        case State::installing:               return "installing";
        case State::install_failed:           return "install_failed";
        case State::restart_pending:          return "restart_pending";
        case State::unavailable_on_this_sku:  return "unavailable_on_this_sku";
        case State::user_skipped:             return "user_skipped";
    }
    return "unknown";
}

}  // namespace mcp::hdcm
