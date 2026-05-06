/*
 * HDCM Installer — 类别 C Windows Optional Feature(ADR-021)。
 *
 * 实装边界(Phase 8-C 结构性骨架):
 *   - 接口骨架: start_install / cancel + 进度回调
 *   - 主 app 走 ShellExecuteEx(verb="runas") 启动 mc_hdcm_helper.exe;
 *     helper.exe 是独立 cmake target,manifest 带 requireAdministrator,
 *     与主 app 同 vendor 签名,无网络能力,通过命名管道与主 app 通信。
 *   - helper.exe 工程 + 命名管道协议留下一轮 cmake target 配置实装。
 *   - 当前 stub: start_install 上报 not_implemented。
 *
 * 用户拒绝 UAC → result=uac_denied → 面板提示重试。
 * 启用成功后 restart_required=Required → 弹"立即重启 / 稍后重启"对话框。
 */

#ifndef MC_PLAYER_HDCM_INSTALLER_C_FEATURE_H_
#define MC_PLAYER_HDCM_INSTALLER_C_FEATURE_H_

#include <functional>
#include <memory>

#include "hdcm/manifest_table.h"
#include "mc-player/mc_player_types.h"

namespace mcp::hdcm {

class InstallerOptionalFeature {
public:
    using ProgressFn = std::function<void(const ComponentManifest&, State, uint32_t /*progress*/)>;

    explicit InstallerOptionalFeature(ProgressFn progress) noexcept;
    ~InstallerOptionalFeature();

    InstallerOptionalFeature(const InstallerOptionalFeature&)            = delete;
    InstallerOptionalFeature& operator=(const InstallerOptionalFeature&) = delete;

    /// 异步 ShellExecuteEx(verb="runas") + 命名管道 IPC + DismApi。
    mc_status_t start_install(const ComponentManifest& m) noexcept;

    void cancel() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp::hdcm

#endif  // MC_PLAYER_HDCM_INSTALLER_C_FEATURE_H_
