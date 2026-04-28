#include "controller/device_lost.h"

namespace mcp::controller {

DeviceLostRecovery::DeviceLostRecovery(DeviceLostHooks hooks) noexcept : hooks_{std::move(hooks)} {}

mc_status_t DeviceLostRecovery::run() noexcept {
    if (hooks_.emit_device_lost_event)        hooks_.emit_device_lost_event();
    if (hooks_.teardown_codec)                hooks_.teardown_codec();
    if (hooks_.teardown_render_chain)         hooks_.teardown_render_chain();
    if (hooks_.rebuild_factory)               hooks_.rebuild_factory();
    if (hooks_.rebuild_device_and_swapchain)  hooks_.rebuild_device_and_swapchain();
    if (hooks_.rebuild_codec)                 hooks_.rebuild_codec();
    if (hooks_.flush_jitter)                  hooks_.flush_jitter();
    if (hooks_.mark_outstanding_invalid)      hooks_.mark_outstanding_invalid();
    if (hooks_.send_pli)                      hooks_.send_pli();
    if (hooks_.emit_device_recovered_event)   hooks_.emit_device_recovered_event();
    return MC_OK;
}

}  // namespace mcp::controller
