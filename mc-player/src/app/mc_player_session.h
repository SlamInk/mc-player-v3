/*
 * Session — 公开 ABI 句柄背后的实例。
 *
 * 一个 mc_player_t = 一个 Session = 一条流。
 * Session 持有 Controller，Controller 编排 Transport / MediaCore / Render 三大子系统。
 *
 * 本文件只暴露 Session 的最小接口，让 mc_player_api.cpp 把 C ABI 调用映射进来。
 * 真正的状态机 / 数据流在 Controller 层。
 */

#ifndef MC_PLAYER_APP_MC_PLAYER_SESSION_H_
#define MC_PLAYER_APP_MC_PLAYER_SESSION_H_

#include <Windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "mc-player/mc_player.h"
#include "controller/controller.h"

namespace mcp::app {

class Session {
public:
    Session();
    ~Session();

    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;

    mc_status_t open(const mc_open_options_t& options) noexcept;
    void        close() noexcept;

    mc_status_t set_render_target(HWND hwnd) noexcept;
    mc_status_t set_event_callback(mc_event_callback_fn cb, void* user) noexcept;

    mc_status_t get_state(mc_state_t& out) const noexcept;
    mc_status_t get_stats(mc_stats_t& inout) const noexcept;
    mc_status_t get_stream_info(mc_stream_info_t& inout) const noexcept;

    mc_status_t tick_ui() noexcept;
    mc_status_t set_show_stats(bool show) noexcept;
    mc_status_t set_show_add_modal(bool show) noexcept;

private:
    void emit_event(const mc_event_t& evt) noexcept;

    // 把 controller::Event 翻译成公开 mc_event_t 并 emit。
    void on_controller_event(const controller::Event& evt) noexcept;

    mutable std::mutex                  mu_;
    std::unique_ptr<controller::Controller> controller_;

    std::atomic<mc_event_callback_fn>   event_cb_{nullptr};
    void*                               event_user_{nullptr};

    mc_state_t                          state_{MC_STATE_IDLE};
};

}  // namespace mcp::app

// 公开 ABI 不透明句柄定义（C 端见到的 mc_player_s* 是 mcp::app::Session*）。
struct mc_player_s {
    ::mcp::app::Session impl;
};

#endif  // MC_PLAYER_APP_MC_PLAYER_SESSION_H_
