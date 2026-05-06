/*
 * HDCM Installer — 类别 B Microsoft Store 媒体扩展(ADR-021)。
 *
 * 实装边界(Phase 8-B 结构性骨架):
 *   - 接口骨架: start_install / cancel + 进度回调
 *   - C++/WinRT (Windows.Services.Store / Windows.Management.Deployment) 集成
 *     需要 cppwinrt header 生成与 winrt namespace import,与 mc-player 现有 C++
 *     代码风格分离;留下一轮 cmake target 配置迭代实装
 *   - 当前 stub: start_install 上报 not_implemented;detector 已在 8-A 留 unknown 默认
 *
 * SKU 限制(IoT LTSC / Server / 无 Store 客户端): detector 端识别后归
 *   unavailable_on_this_sku,本 installer 不应被调用。
 */

#ifndef MC_PLAYER_HDCM_INSTALLER_B_STORE_H_
#define MC_PLAYER_HDCM_INSTALLER_B_STORE_H_

#include <functional>
#include <memory>

#include "hdcm/manifest_table.h"
#include "mc-player/mc_player_types.h"

namespace mcp::hdcm {

class InstallerStoreExtension {
public:
    using ProgressFn = std::function<void(const ComponentManifest&, State, uint32_t /*progress 0..100*/)>;

    explicit InstallerStoreExtension(ProgressFn progress) noexcept;
    ~InstallerStoreExtension();

    InstallerStoreExtension(const InstallerStoreExtension&)            = delete;
    InstallerStoreExtension& operator=(const InstallerStoreExtension&) = delete;

    /// 异步 StoreContext::RequestDownloadAndInstallStorePackagesAsync。
    mc_status_t start_install(const ComponentManifest& m) noexcept;

    void cancel() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp::hdcm

#endif  // MC_PLAYER_HDCM_INSTALLER_B_STORE_H_
