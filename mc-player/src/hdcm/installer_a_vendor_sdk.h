/*
 * HDCM Installer — 类别 A vendor SDK 下载(ADR-016)。
 *
 * 实装边界(Phase 8-A):
 *   - 接口骨架: start_install / cancel / progress 回调
 *   - WinHTTP 异步 GET + SHA-256 + WinTrust Authenticode + 解压留 Phase 8-A 完整版
 *   - 当前 stub: start_install 上报 not_implemented,与 ui_panel 解耦
 */

#ifndef MC_PLAYER_HDCM_INSTALLER_A_VENDOR_SDK_H_
#define MC_PLAYER_HDCM_INSTALLER_A_VENDOR_SDK_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "hdcm/manifest_table.h"
#include "mc-player/mc_player_types.h"

namespace mcp::hdcm {

class InstallerVendorSdk {
public:
    using ProgressFn = std::function<void(const ComponentManifest&, State, uint32_t /*progress 0..100*/)>;

    explicit InstallerVendorSdk(ProgressFn progress) noexcept;
    ~InstallerVendorSdk();

    InstallerVendorSdk(const InstallerVendorSdk&)            = delete;
    InstallerVendorSdk& operator=(const InstallerVendorSdk&) = delete;

    /// 异步下载 + 校验 + 解压;失败时 progress 回调 state=install_failed。
    mc_status_t start_install(const ComponentManifest& m) noexcept;

    /// 用户取消(WinHTTP request abort)。
    void cancel() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp::hdcm

#endif  // MC_PLAYER_HDCM_INSTALLER_A_VENDOR_SDK_H_
