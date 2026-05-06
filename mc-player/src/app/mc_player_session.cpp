#include "app/mc_player_session.h"

#include "pal/etw_provider.h"
#include "pal/log.h"
#include "pal/metric.h"

namespace mcp::app {

namespace {

// 按 metric 名拉 quantile 填 v2 stats 字段；metric Registry 是单例，懒创建保证调用侧安全。
inline uint64_t hist_p95_ns(std::string_view name) noexcept {
    auto& h = mcp::pal::metric::Registry::instance().histogram(name);
    const auto q = h.quantile(0.95);
    return q < 0 ? 0u : static_cast<uint64_t>(q);
}

inline uint64_t counter_value(std::string_view name) noexcept {
    return mcp::pal::metric::Registry::instance().counter(name).value();
}

}  // namespace

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
    {
        std::scoped_lock lk{mu_};
        if (auto s = controller_->get_stats(inout); s != MC_OK) return s;
    }

    // Phase 0 v2 字段填充（性能量度规范 §11.4）—— 从 metric Registry 拉 9 段 timer p95
    // + 6 bit gate counter + preset/probe gauge。controller::get_stats 已填 v1 字段
    // 与部分 v2 字段（如 decoder_kind 通过 stream_info），此处补齐 metric 端。
    inout.stage_udp_rx_p95_ns           = hist_p95_ns("mc.stage.udp_rx_ns");
    inout.stage_jitter_dwell_p95_ns     = hist_p95_ns("mc.stage.jitter_dwell_ns");
    inout.stage_depack_p95_ns           = hist_p95_ns("mc.stage.depack_ns");
    inout.stage_decode_alloc_p95_ns     = hist_p95_ns("mc.stage.decode_alloc_ns");
    inout.stage_decode_actual_p95_ns    = hist_p95_ns("mc.stage.decode_actual_ns");
    inout.stage_decode_output_p95_ns    = hist_p95_ns("mc.stage.decode_output_ns");
    inout.stage_upload_p95_ns           = hist_p95_ns("mc.stage.upload_ns");
    inout.stage_yuv2rgb_p95_ns          = hist_p95_ns("mc.stage.yuv2rgb_ns");
    inout.stage_present_queue_p95_ns    = hist_p95_ns("mc.stage.present_queue_ns");

    inout.e2e_latency_p95_ns            = hist_p95_ns("mc.e2e.latency_ns");
    inout.e2e_client_internal_p95_ns    = hist_p95_ns("mc.e2e.client_internal_ns");

    // Probe 完成时间 p95（capability_probe §3 ~ §5；Phase 9.0 实装 probe 后真实 record）。
    inout.probe_hardware_complete_p95_ns = hist_p95_ns("mc.probe.hardware_complete_ms");
    inout.probe_network_complete_p95_ns  = hist_p95_ns("mc.probe.network_complete_ms");
    inout.probe_encoder_complete_p95_ns  = hist_p95_ns("mc.probe.encoder_complete_ms");
    inout.probe_render_complete_p95_ns   = hist_p95_ns("mc.probe.render_complete_ms");

    // Preset 状态（plan Phase 9 实装；Phase 0 默认 NONE，selector 未启动）。
    inout.preset_active_id              = static_cast<mc_preset_id_t>(
        mcp::pal::metric::Registry::instance().gauge("mc.preset.active_id").value());
    inout.preset_reload_count           = counter_value("mc.preset.reload_count");
    inout.preset_downgrade_count        = counter_value("mc.preset.downgrade_count");
    inout.preset_upgrade_count          = counter_value("mc.preset.upgrade_count");
    inout.preset_apply_atomic_violation_count = counter_value("mc.preset.apply_atomic_violation_count");
    inout.preset_apply_partial_count    = counter_value("mc.preset.apply_partial_count");
    inout.preset_apply_failure_count    = counter_value("mc.preset.apply_failure_count");

    return MC_OK;
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
