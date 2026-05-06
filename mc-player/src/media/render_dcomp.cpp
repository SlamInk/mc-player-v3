#include "media/render_dcomp.h"

#include "pal/error.h"
#include "pal/log.h"
#include "pal/metric.h"

namespace mcp::media {

mc_status_t DcompRoot::create(HWND /*hwnd*/) noexcept {
    // TODO: DCompositionCreateDevice2 → CreateTargetForHwnd(hwnd, TRUE)
    //       CreateVisual ×3 → SetContent / SetTransform → root_visual_.AddVisual(...)
    return MC_OK;
}

void DcompRoot::destroy() noexcept {
    status_visual_.Reset();
    hud_visual_.Reset();
    video_visual_.Reset();
    root_visual_.Reset();
    target_.Reset();
    device_.Reset();
}

mc_status_t DcompRoot::commit() noexcept {
    if (!device_) return MC_ERR_INVALID_STATE;
    HRESULT hr = device_->Commit();
    if (FAILED(hr)) return pal::status_from_hresult(hr);
    // 性能量度规范 §7.1 mc.render.dcomp_commit_count
    // (与 mc.render.present_fps 大致同步,差额 < 1%)。
    // 此处是全仓库唯一 IDCompositionDevice::Commit 调用点 — ADD §5.10.5 单点权威。
    pal::metric::Registry::instance()
        .counter("mc.render.dcomp_commit_count").inc();
    return MC_OK;
}

}  // namespace mcp::media
