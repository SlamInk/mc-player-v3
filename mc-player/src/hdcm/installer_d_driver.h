/*
 * HDCM Installer — 类别 D GPU driver 阈值检测 + 跳浏览器引导(ADR-021)。
 *
 * 实装边界(Phase 8-D):
 *   - WMI Win32_VideoController.DriverVersion 查询 + 四段版本解析
 *   - vs manifest_table 阈值比较(driver_min_*)→ 落后即上报 driver_below_threshold
 *   - "前往 vendor 官网"按钮 → ShellExecuteEx(verb="open", lpFile=vendor_url)
 *   - mc-player 进程内不运行 driver setup.exe(工程边界)
 *   - 不监控外部 driver 安装结果;下次启动重新 detect
 */

#ifndef MC_PLAYER_HDCM_INSTALLER_D_DRIVER_H_
#define MC_PLAYER_HDCM_INSTALLER_D_DRIVER_H_

#include <cstdint>
#include <memory>
#include <string>

#include "hdcm/manifest_table.h"
#include "mc-player/mc_player_types.h"

namespace mcp::hdcm {

struct DriverInfo {
    std::string raw_version;     // "31.0.101.5333" 形式
    uint32_t    major    = 0;
    uint32_t    minor    = 0;
    uint32_t    build    = 0;
    uint32_t    revision = 0;
    bool        valid    = false;
};

class InstallerGpuDriver {
public:
    InstallerGpuDriver() noexcept;
    ~InstallerGpuDriver();

    InstallerGpuDriver(const InstallerGpuDriver&)            = delete;
    InstallerGpuDriver& operator=(const InstallerGpuDriver&) = delete;

    /// 探测当前 adapter driver 版本 vs manifest 阈值。
    /// 返回 true = 已达 / 高于阈值;false = 低于阈值需更新。
    [[nodiscard]] bool meets_threshold(const ComponentManifest& m, DriverInfo* out_info = nullptr) noexcept;

    /// 跳 vendor 官网驱动页;mc-player 不监控安装结果。
    mc_status_t open_vendor_url(const ComponentManifest& m) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp::hdcm

#endif  // MC_PLAYER_HDCM_INSTALLER_D_DRIVER_H_
