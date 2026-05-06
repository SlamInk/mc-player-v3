#include "hdcm/installer_a_vendor_sdk.h"

#include "pal/error.h"
#include "pal/log.h"
#include "pal/metric.h"

namespace mcp::hdcm {

struct InstallerVendorSdk::Impl {
    ProgressFn progress;
};

InstallerVendorSdk::InstallerVendorSdk(ProgressFn progress) noexcept
    : impl_{std::make_unique<Impl>()} {
    impl_->progress = std::move(progress);
}

InstallerVendorSdk::~InstallerVendorSdk() = default;

mc_status_t InstallerVendorSdk::start_install(const ComponentManifest& m) noexcept {
    // Phase 8-A 完整版: WinHTTPOpen → SetTimeouts → SendRequest 异步 → 全量到 temp 文件
    //   → SHA-256 (BCryptCreateHash) → WinTrust (WinVerifyTrust)
    //   → 解压 / 复制到 %LOCALAPPDATA%\mc-player\sdk\<vendor>\<version>\
    //   → progress 回调 state 变迁: installing → already_installed
    //
    // 当前 stub: 上报 install_failed 让 ui_panel 提示"待 Phase 8-A 完整实装"。
    pal::metric::Registry::instance()
        .counter("mc.hdcm.install_attempt_count.A.not_implemented").inc();
    if (impl_->progress) impl_->progress(m, State::install_failed, 0);
    MCP_LOGF(pal::LogLevel::warn,
             "InstallerVendorSdk: start_install id=%s — Phase 8-A 完整实装(WinHTTP + SHA-256 + WinTrust)待补",
             m.id);
    return MC_ERR_UNSUPPORTED;
}

void InstallerVendorSdk::cancel() noexcept {
    // Phase 8-A: WinHttpCloseHandle on active request handle。
}

}  // namespace mcp::hdcm
