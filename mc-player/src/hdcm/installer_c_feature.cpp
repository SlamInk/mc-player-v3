#include "hdcm/installer_c_feature.h"

#include "pal/error.h"
#include "pal/log.h"
#include "pal/metric.h"

namespace mcp::hdcm {

struct InstallerOptionalFeature::Impl {
    ProgressFn progress;
};

InstallerOptionalFeature::InstallerOptionalFeature(ProgressFn progress) noexcept
    : impl_{std::make_unique<Impl>()} {
    impl_->progress = std::move(progress);
}

InstallerOptionalFeature::~InstallerOptionalFeature() = default;

mc_status_t InstallerOptionalFeature::start_install(const ComponentManifest& m) noexcept {
    // Phase 8-C 完整版:
    //   1. 创建命名管道 \\.\pipe\mc-player\hdcm-helper-<pid>
    //   2. ShellExecuteEx({lpVerb=L"runas", lpFile=L"mc_hdcm_helper.exe",
    //                      lpParameters=pipe_name})
    //   3. UAC 弹窗 → 用户同意 / 拒绝
    //   4. helper.exe 启动后连管道, 接 EnableFeature 请求 → DismApi:
    //      DismOpenSession + DismEnableFeature(L"MediaPlayback", ...) +
    //      DismGetFeatureInfo
    //   5. 响应 hr + restart_required + error_message;主 app 弹"立即重启 / 稍后重启"
    //
    // 当前 stub: 上报 install_failed,等 helper.exe 工程 + 协议落地后实装。
    pal::metric::Registry::instance()
        .counter("mc.hdcm.install_attempt_count.C.not_implemented").inc();
    if (impl_->progress) impl_->progress(m, State::install_failed, 0);
    MCP_LOGF(pal::LogLevel::warn,
             "InstallerOptionalFeature: start_install feature=%s — Phase 8-C helper.exe + DismApi 实装待补",
             m.feature_name);
    return MC_ERR_UNSUPPORTED;
}

void InstallerOptionalFeature::cancel() noexcept {
    // Phase 8-C: 关闭命名管道 + 等 helper.exe 退出。
}

}  // namespace mcp::hdcm
