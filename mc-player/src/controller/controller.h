/*
 * Controller — 生命周期 FSM、Adapter 智能选择、Device Lost 全恢复。
 *
 * 职责（ADD §3.1 Controller 层）：
 *   1. 解析 mc_open_options_t；选 transport（RTSP UDP / TCP）。
 *   2. 启动 Transport Session；订阅 RTP 数据流。
 *   3. 启动 Media Core（jitter buffer、depack、codec bridge、frame validity gate、render、audio）。
 *   4. 监听 device-lost / network down / adapter switch；驱动恢复路径。
 *   5. 把内部事件聚合后通过 EventSink 上抛到 Session 层。
 *
 * 本头文件只暴露与 Session 交互的接口；具体子系统组合在 .cpp 中实现。
 */

#ifndef MC_PLAYER_CONTROLLER_CONTROLLER_H_
#define MC_PLAYER_CONTROLLER_CONTROLLER_H_

#include <Windows.h>

#include <functional>
#include <memory>
#include <string>

#include "mc-player/mc_player.h"

namespace mcp::controller {

struct Event {
    mc_event_type_t type{};
    mc_state_t      new_state{MC_STATE_IDLE};
    mc_state_t      prev_state{MC_STATE_IDLE};
    mc_status_t     error_code{MC_OK};
    std::string     error_message;          // 内部存储，emit 时映射到 mc_event_t.error_message
    mc_stream_info_t stream_info{};         // 仅 STREAM_INFO 事件填充
    bool            stream_info_valid{false};
};

using EventSink = std::function<void(const Event&)>;

class Controller {
public:
    Controller();
    ~Controller();

    Controller(const Controller&)            = delete;
    Controller& operator=(const Controller&) = delete;

    void set_event_sink(EventSink sink) noexcept;

    mc_status_t open(const mc_open_options_t& options) noexcept;
    void        close() noexcept;

    mc_status_t set_render_target(HWND hwnd) noexcept;

    mc_status_t get_state(mc_state_t& out) const noexcept;
    mc_status_t get_stats(mc_stats_t& inout) const noexcept;
    mc_status_t get_stream_info(mc_stream_info_t& inout) const noexcept;

    /// 60Hz UI tick：推进动画时间基；idle/connecting/error stage 强制 redraw。
    mc_status_t tick_ui() noexcept;
    mc_status_t set_show_stats(bool show) noexcept;
    mc_status_t set_show_add_modal(bool show) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp::controller

#endif  // MC_PLAYER_CONTROLLER_CONTROLLER_H_
