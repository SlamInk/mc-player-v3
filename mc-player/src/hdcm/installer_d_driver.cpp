#include "hdcm/installer_d_driver.h"

#include <Windows.h>
#include <shellapi.h>

#include <cstdio>

#include "pal/error.h"
#include "pal/log.h"
#include "pal/metric.h"

#pragma comment(lib, "shell32.lib")

namespace mcp::hdcm {

namespace {

// 解析 "31.0.101.5333" 形四段版本。
bool parse_version(const std::string& s, DriverInfo& out) noexcept {
    out.raw_version = s;
    int rc = ::sscanf_s(s.c_str(), "%u.%u.%u.%u",
                          &out.major, &out.minor, &out.build, &out.revision);
    out.valid = (rc == 4);
    return out.valid;
}

// 与阈值比较: (major, minor, build, revision) 字典序 >= 阈值即通过。
// driver_min_* 全 0 表示 "未配置阈值",视为通过(不阻止 user)。
bool meets_threshold_value(const DriverInfo& info, const ComponentManifest& m) noexcept {
    if (m.driver_min_major == 0 && m.driver_min_minor == 0 &&
        m.driver_min_build == 0 && m.driver_min_revision == 0) {
        return true;     // 未配置阈值
    }
    if (info.major != m.driver_min_major) return info.major > m.driver_min_major;
    if (info.minor != m.driver_min_minor) return info.minor > m.driver_min_minor;
    if (info.build != m.driver_min_build) return info.build > m.driver_min_build;
    return info.revision >= m.driver_min_revision;
}

}  // namespace

struct InstallerGpuDriver::Impl {
    // Phase 8-D 完整版: WMI IWbemServices ExecQuery
    //   "SELECT DriverVersion, AdapterCompatibility FROM Win32_VideoController
    //    WHERE PNPDeviceID LIKE '%VEN_<vendor>%'"
    //
    // 当前简化: DXGI 已经枚举了 adapter description,driver version 由
    // dxgi_caps_probe 暴露(待 8-D 后续 commit 接入)。本骨架返默认 valid=false
    // 让 detect 视为 unknown 不阻塞启动。
    DriverInfo query_driver_version(uint32_t /*vendor_id*/) noexcept {
        DriverInfo info;
        // TODO(8-D): 调 IWbemServices::ExecQuery 拿真实 DriverVersion
        info.valid = false;
        return info;
    }
};

InstallerGpuDriver::InstallerGpuDriver() noexcept
    : impl_{std::make_unique<Impl>()} {}

InstallerGpuDriver::~InstallerGpuDriver() = default;

bool InstallerGpuDriver::meets_threshold(const ComponentManifest& m, DriverInfo* out_info) noexcept {
    DriverInfo info = impl_->query_driver_version(m.target_vendor_id);
    if (out_info) *out_info = info;
    if (!info.valid) {
        // 查询失败 → 默认 true(不主动报告 below_threshold)。
        return true;
    }
    const bool meets = meets_threshold_value(info, m);
    char metric_name[96];
    std::snprintf(metric_name, sizeof(metric_name),
                  "mc.hdcm.driver_below_threshold.%s", m.id);
    pal::metric::Registry::instance().gauge(metric_name).set(meets ? 0 : 1);
    if (!meets) {
        MCP_LOGF(pal::LogLevel::info,
                 "InstallerGpuDriver: %s driver %s < threshold %u.%u.%u.%u",
                 m.id, info.raw_version.c_str(),
                 m.driver_min_major, m.driver_min_minor,
                 m.driver_min_build, m.driver_min_revision);
    }
    return meets;
}

mc_status_t InstallerGpuDriver::open_vendor_url(const ComponentManifest& m) noexcept {
    if (!m.download_url || !*m.download_url) return MC_ERR_INVALID_ARG;
    wchar_t wurl[2048]{};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, m.download_url, -1, wurl, 2048);
    if (n <= 0) return MC_ERR_INTERNAL;
    HINSTANCE rc = ::ShellExecuteW(nullptr, L"open", wurl, nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(rc) <= 32) {
        MCP_LOGF(pal::LogLevel::warn,
                 "InstallerGpuDriver: ShellExecuteW failed for %s (rc=%lld)",
                 m.id, reinterpret_cast<long long>(rc));
        return MC_ERR_INTERNAL;
    }
    pal::metric::Registry::instance()
        .counter("mc.hdcm.driver_url_opened_count").inc();
    MCP_LOGF(pal::LogLevel::info,
             "InstallerGpuDriver: opened vendor url for %s", m.id);
    return MC_OK;
}

}  // namespace mcp::hdcm
