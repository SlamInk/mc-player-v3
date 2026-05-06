#include "hdcm/detector.h"

#include <Windows.h>

#include "pal/log.h"
#include "pal/metric.h"

namespace mcp::hdcm {

namespace {

bool dll_loadable(const char* dll_name_utf8) noexcept {
    if (!dll_name_utf8 || !*dll_name_utf8) return false;
    wchar_t wname[MAX_PATH]{};
    ::MultiByteToWideChar(CP_UTF8, 0, dll_name_utf8, -1, wname, MAX_PATH);
    HMODULE h = ::LoadLibraryW(wname);
    if (!h) return false;
    ::FreeLibrary(h);
    return true;
}

void emit_state_metric(const ComponentManifest& m, State s) noexcept {
    char name[128];
    std::snprintf(name, sizeof(name),
                  "mc.hdcm.component.%s.state", m.id);
    pal::metric::Registry::instance().gauge(name).set(static_cast<int64_t>(s));
}

}  // namespace

ComponentSnapshot detect_component(const ComponentManifest& m, uint32_t adapter_vendor_id) noexcept {
    ComponentSnapshot snap;
    snap.manifest           = &m;
    snap.detected_vendor_id = adapter_vendor_id;

    switch (m.category) {
        case Category::A_VendorSdk: {
            // VendorId 不匹配(adapter 与 SDK 厂商不一致) → unavailable_on_this_sku
            if (m.target_vendor_id != 0 && m.target_vendor_id != adapter_vendor_id) {
                snap.state = State::unavailable_on_this_sku;
                break;
            }
            snap.state = dll_loadable(m.dll_name) ? State::already_installed
                                                    : State::installable;
            break;
        }
        case Category::B_StoreExtension:
            // Phase 8-B: PackageManager::FindPackagesForUser 查找 PackageFamilyName。
            // 当前 stub: unknown,让 ui_panel 隐藏入口。
            snap.state = State::unknown;
            break;
        case Category::C_OptionalFeature:
            // Phase 8-C: DismApi 查询 feature 状态。当前 stub: unknown。
            snap.state = State::unknown;
            break;
        case Category::D_GpuDriver:
            // Phase 8-D: WMI Win32_VideoController.DriverVersion 解析。当前 stub: unknown。
            snap.state = State::unknown;
            break;
    }

    emit_state_metric(m, snap.state);
    MCP_LOGF(pal::LogLevel::info,
             "Hdcm detect: id=%s category=%s state=%s vendor=0x%04X",
             m.id, category_label(m.category), state_label(snap.state),
             adapter_vendor_id);
    return snap;
}

std::vector<ComponentSnapshot> detect_all_components(uint32_t adapter_vendor_id) noexcept {
    pal::metric::ScopedTimer t{
        pal::metric::Registry::instance().timer("mc.hdcm.detect_all_ns")};
    std::vector<ComponentSnapshot> out;
    out.reserve(manifest_table().size());
    for (const auto& m : manifest_table()) {
        out.push_back(detect_component(m, adapter_vendor_id));
    }
    return out;
}

}  // namespace mcp::hdcm
