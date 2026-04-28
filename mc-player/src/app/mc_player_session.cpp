#include "app/mc_player_session.h"

#include "pal/log.h"

namespace mcp::app {

namespace {

mc_status_t validate_struct_header(const void* p, std::size_t expected_min_size) noexcept {
    if (!p) return MC_ERR_INVALID_ARG;
    const std::size_t* sz = reinterpret_cast<const std::size_t*>(p);
    if (*sz < expected_min_size) return MC_ERR_INVALID_ARG;
    return MC_OK;
}

}  // namespace

Session::Session() : controller_{std::make_unique<controller::Controller>()} {
    controller_->set_event_sink([this](const controller::Event& e) {
        on_controller_event(e);
    });
}

Session::~Session() {
    close();
}

mc_status_t Session::open(const mc_open_options_t& options) noexcept {
    if (mc_status_t s = validate_struct_header(&options, sizeof(mc_open_options_t)); s != MC_OK) {
        return s;
    }
    std::scoped_lock lk{mu_};
    return controller_->open(options);
}

void Session::close() noexcept {
    std::scoped_lock lk{mu_};
    controller_->close();
}

mc_status_t Session::set_render_target(HWND hwnd) noexcept {
    std::scoped_lock lk{mu_};
    return controller_->set_render_target(hwnd);
}

mc_status_t Session::set_event_callback(mc_event_callback_fn cb, void* user) noexcept {
    event_user_ = user;
    event_cb_.store(cb, std::memory_order_release);
    return MC_OK;
}

mc_status_t Session::get_state(mc_state_t& out) const noexcept {
    std::scoped_lock lk{mu_};
    return controller_->get_state(out);
}

mc_status_t Session::get_stats(mc_stats_t& inout) const noexcept {
    if (inout.struct_size < sizeof(mc_stats_t)) {
        return MC_ERR_INVALID_ARG;
    }
    std::scoped_lock lk{mu_};
    return controller_->get_stats(inout);
}

mc_status_t Session::get_stream_info(mc_stream_info_t& inout) const noexcept {
    if (inout.struct_size < sizeof(mc_stream_info_t)) {
        return MC_ERR_INVALID_ARG;
    }
    std::scoped_lock lk{mu_};
    return controller_->get_stream_info(inout);
}

mc_status_t Session::tick_ui() noexcept {
    std::scoped_lock lk{mu_};
    return controller_->tick_ui();
}

mc_status_t Session::set_show_stats(bool show) noexcept {
    std::scoped_lock lk{mu_};
    return controller_->set_show_stats(show);
}

mc_status_t Session::set_show_add_modal(bool show) noexcept {
    std::scoped_lock lk{mu_};
    return controller_->set_show_add_modal(show);
}

void Session::emit_event(const mc_event_t& evt) noexcept {
    auto cb = event_cb_.load(std::memory_order_acquire);
    if (cb) cb(event_user_, &evt);
}

void Session::on_controller_event(const controller::Event& evt) noexcept {
    if (evt.type == MC_EVENT_STATE_CHANGED) {
        state_ = evt.new_state;
    }

    mc_event_t out{};
    out.struct_size    = sizeof(mc_event_t);
    out.struct_version = MC_EVENT_VERSION;
    out.type           = evt.type;
    out.new_state      = evt.new_state;
    out.prev_state     = evt.prev_state;
    out.error_code     = evt.error_code;
    out.error_message  = evt.error_message.empty() ? nullptr : evt.error_message.c_str();
    if (evt.stream_info_valid) {
        out.stream_info = &evt.stream_info;
    }
    emit_event(out);
}

}  // namespace mcp::app
