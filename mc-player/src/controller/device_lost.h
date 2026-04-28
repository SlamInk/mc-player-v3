/*
 * Device Lost 全恢复（ADD §7.2.2 / ADR-014）。
 *
 * 触发：任一 D3D11 / DXGI 调用返回 DEVICE_REMOVED / DEVICE_RESET / DEVICE_HUNG。
 *
 * 恢复序列：
 *   1. 销毁解码器 / swap chain / view 链；ImmediateContext::ClearState；释放 D3D11 device
 *   2. 重建 DXGI factory（旧 factory IsCurrent() 返回 false 后 stale，强烈建议重建）
 *   3. LUID 找回同 adapter（失败则智能重选）
 *   4. 重建 device / MFT 或软解 / swap chain
 *   5. 清空 jitter（数据依赖旧 device）
 *   6. 标记所有 outstanding 帧 invalid（旧 fence 失效，由 Frame Validity Gate 丢弃）
 *   7. PLI 请求新 I 帧
 *   8. 触发 device-lost / device-recovered 事件序列
 */

#ifndef MC_PLAYER_CONTROLLER_DEVICE_LOST_H_
#define MC_PLAYER_CONTROLLER_DEVICE_LOST_H_

#include <functional>

#include "mc-player/mc_player_types.h"

namespace mcp::controller {

struct DeviceLostHooks {
    std::function<void()> teardown_render_chain;
    std::function<void()> teardown_codec;
    std::function<void()> rebuild_factory;
    std::function<void()> rebuild_device_and_swapchain;
    std::function<void()> rebuild_codec;
    std::function<void()> flush_jitter;
    std::function<void()> mark_outstanding_invalid;
    std::function<void()> send_pli;
    std::function<void()> emit_device_lost_event;
    std::function<void()> emit_device_recovered_event;
};

class DeviceLostRecovery {
public:
    explicit DeviceLostRecovery(DeviceLostHooks hooks) noexcept;
    mc_status_t run() noexcept;

private:
    DeviceLostHooks hooks_;
};

}  // namespace mcp::controller

#endif  // MC_PLAYER_CONTROLLER_DEVICE_LOST_H_
