#include "media/render_dcomp.h"

#include "pal/error.h"
#include "pal/log.h"

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
    return SUCCEEDED(hr) ? MC_OK : pal::status_from_hresult(hr);
}

}  // namespace mcp::media
