#include "hdcm/installer_b_store.h"

#include "pal/error.h"
#include "pal/log.h"
#include "pal/metric.h"

namespace mcp::hdcm {

struct InstallerStoreExtension::Impl {
    ProgressFn progress;
};

InstallerStoreExtension::InstallerStoreExtension(ProgressFn progress) noexcept
    : impl_{std::make_unique<Impl>()} {
    impl_->progress = std::move(progress);
}

InstallerStoreExtension::~InstallerStoreExtension() = default;

mc_status_t InstallerStoreExtension::start_install(const ComponentManifest& m) noexcept {
    // Phase 8-B 完整版:
    //   - cppwinrt 集成: cmake add target_compile_definitions WINRT_LEAN_AND_MEAN
    //     + 头生成 winrt/Windows.Services.Store.h / winrt/Windows.Foundation.h
    //   - 调用流:
    //     auto context = winrt::Windows::Services::Store::StoreContext::GetDefault();
    //     auto op = context.RequestDownloadAndInstallStorePackagesAsync({m.package_family});
    //     op.Progress([&](auto, auto p) { progress(m, State::installing, p.PackageDownloadProgress); });
    //     auto result = co_await op;
    //     progress(m, result.OverallState() == ... ? already_installed : install_failed, 100);
    //
    // 当前 stub: 上报 install_failed,等待下一轮实装。
    pal::metric::Registry::instance()
        .counter("mc.hdcm.install_attempt_count.B.not_implemented").inc();
    if (impl_->progress) impl_->progress(m, State::install_failed, 0);
    MCP_LOGF(pal::LogLevel::warn,
             "InstallerStoreExtension: start_install id=%s — Phase 8-B C++/WinRT 实装待补",
             m.id);
    return MC_ERR_UNSUPPORTED;
}

void InstallerStoreExtension::cancel() noexcept {
    // Phase 8-B: IAsyncOperation::Cancel。
}

}  // namespace mcp::hdcm
