/*
 * 公开 C ABI 实现 — 把 mc_xxx() 调用映射到 Session / Controller。
 *
 * 所有函数 noexcept；任何异常在边界吃掉换成 MC_ERR_INTERNAL。
 */

#include "mc-player/mc_player.h"

#include <share.h>

#include <atomic>
#include <cstdio>
#include <new>

#include "app/mc_player_session.h"
#include "pal/clock.h"
#include "pal/com_apartment.h"
#include "pal/error.h"
#include "pal/etw_provider.h"
#include "pal/log.h"
#include "pal/metric.h"
#include "pal/mf_runtime.h"
#include "pal/socket_iocp.h"

namespace {

struct GlobalState {
    std::atomic<int>            refcount{0};
    std::unique_ptr<mcp::pal::ComApartment>   com_main;
    std::unique_ptr<mcp::pal::WsaRuntimeRef>  wsa;
    std::unique_ptr<mcp::pal::MfRuntimeRef>   mf;
    FILE*                                     log_file = nullptr;
};

GlobalState& global_state() noexcept {
    static GlobalState s;
    return s;
}

template <typename Fn>
mc_status_t guarded(Fn&& fn) noexcept {
    try {
        return fn();
    } catch (const std::bad_alloc&) {
        return MC_ERR_OUT_OF_MEMORY;
    } catch (...) {
        return MC_ERR_INTERNAL;
    }
}

}  // namespace

extern "C" {

MC_API mc_status_t mc_global_init(const mc_init_options_t* options) {
    return guarded([&] {
        if (options) {
            if (options->struct_size < sizeof(mc_init_options_t)) {
                return MC_ERR_INVALID_ARG;
            }
        }
        auto& g = global_state();
        if (g.refcount.fetch_add(1, std::memory_order_acq_rel) == 0) {
            mcp::pal::Clock::init();
            // Phase 0：性能量度规范 §9.3 ETW Provider 注册（logman query providers mc-player 可见）。
            mcp::pal::etw::register_provider();

            mcp::pal::LogConfig log_cfg;
            const char* log_file_path = nullptr;
            if (options) {
                log_cfg.enable_etw      = options->enable_etw_tracing != 0;
                log_cfg.log_to_debugger = options->log_to_debugger    != 0;
                log_cfg.ringbuf_dir     = options->log_ringbuf_dir;
                if (options->struct_size >= sizeof(mc_init_options_t) &&
                    options->struct_version >= 2) {
                    log_file_path = options->log_file_path;
                }
            }
            mcp::pal::log_init(log_cfg);
            if (log_file_path && *log_file_path) {
                // shared write，让 host 也能并行写同一文件。
                g.log_file = ::_fsopen(log_file_path, "a", _SH_DENYNO);
                if (g.log_file) mcp::pal::log_attach_file(g.log_file);
            }

            g.com_main = std::make_unique<mcp::pal::ComApartment>(mcp::pal::ComApartment::Model::mta);
            g.wsa      = std::make_unique<mcp::pal::WsaRuntimeRef>();
            if (!g.wsa->ok()) {
                g.com_main.reset();
                g.wsa.reset();
                g.refcount.fetch_sub(1, std::memory_order_acq_rel);
                return MC_ERR_INTERNAL;
            }
            g.mf = std::make_unique<mcp::pal::MfRuntimeRef>();
            if (g.mf->status() != MC_OK) {
                const mc_status_t s = g.mf->status();
                g.mf.reset();
                g.wsa.reset();
                g.com_main.reset();
                g.refcount.fetch_sub(1, std::memory_order_acq_rel);
                return s;
            }
            MCP_LOG_INFO("mc_global_init: runtime online");
        }
        return MC_OK;
    });
}

MC_API mc_status_t mc_global_shutdown(void) {
    return guarded([&] {
        auto& g = global_state();
        const int prev = g.refcount.fetch_sub(1, std::memory_order_acq_rel);
        if (prev == 1) {
            g.mf.reset();
            g.wsa.reset();
            g.com_main.reset();
            mcp::pal::etw::unregister_provider();
            mcp::pal::log_attach_file(nullptr);
            if (g.log_file) {
                std::fclose(g.log_file);
                g.log_file = nullptr;
            }
            mcp::pal::log_shutdown();
        } else if (prev <= 0) {
            // 反向匹配错误：调用方多调一次 shutdown。复原计数。
            g.refcount.fetch_add(1, std::memory_order_acq_rel);
            return MC_ERR_INVALID_STATE;
        }
        return MC_OK;
    });
}

MC_API mc_status_t mc_create(mc_player_t* out_handle) {
    return guarded([&]() -> mc_status_t {
        if (!out_handle) return MC_ERR_INVALID_ARG;
        if (global_state().refcount.load(std::memory_order_acquire) <= 0) {
            return MC_ERR_INVALID_STATE;
        }
        auto* h = new (std::nothrow) mc_player_s{};
        if (!h) return MC_ERR_OUT_OF_MEMORY;
        *out_handle = h;
        return MC_OK;
    });
}

MC_API mc_status_t mc_destroy(mc_player_t handle) {
    return guarded([&]() -> mc_status_t {
        if (!handle) return MC_ERR_NULL_HANDLE;
        delete handle;
        return MC_OK;
    });
}

MC_API mc_status_t mc_open(mc_player_t handle, const mc_open_options_t* options) {
    return guarded([&]() -> mc_status_t {
        if (!handle)  return MC_ERR_NULL_HANDLE;
        if (!options) return MC_ERR_INVALID_ARG;
        return handle->impl.open(*options);
    });
}

MC_API mc_status_t mc_close(mc_player_t handle) {
    return guarded([&]() -> mc_status_t {
        if (!handle) return MC_ERR_NULL_HANDLE;
        handle->impl.close();
        return MC_OK;
    });
}

MC_API mc_status_t mc_set_render_target(mc_player_t handle, HWND hwnd) {
    return guarded([&]() -> mc_status_t {
        if (!handle) return MC_ERR_NULL_HANDLE;
        return handle->impl.set_render_target(hwnd);
    });
}

MC_API mc_status_t mc_set_event_callback(mc_player_t handle,
                                          mc_event_callback_fn callback,
                                          void* user_data) {
    return guarded([&]() -> mc_status_t {
        if (!handle) return MC_ERR_NULL_HANDLE;
        return handle->impl.set_event_callback(callback, user_data);
    });
}

MC_API mc_status_t mc_get_state(mc_player_t handle, mc_state_t* out_state) {
    return guarded([&]() -> mc_status_t {
        if (!handle || !out_state) return MC_ERR_INVALID_ARG;
        return handle->impl.get_state(*out_state);
    });
}

MC_API mc_status_t mc_get_stats(mc_player_t handle, mc_stats_t* inout_stats) {
    return guarded([&]() -> mc_status_t {
        if (!handle || !inout_stats) return MC_ERR_INVALID_ARG;
        return handle->impl.get_stats(*inout_stats);
    });
}

MC_API mc_status_t mc_get_stream_info(mc_player_t handle, mc_stream_info_t* inout_info) {
    return guarded([&]() -> mc_status_t {
        if (!handle || !inout_info) return MC_ERR_INVALID_ARG;
        return handle->impl.get_stream_info(*inout_info);
    });
}

MC_API mc_status_t mc_tick_ui(mc_player_t handle) {
    return guarded([&]() -> mc_status_t {
        if (!handle) return MC_ERR_NULL_HANDLE;
        return handle->impl.tick_ui();
    });
}

MC_API mc_status_t mc_ui_set_show_stats(mc_player_t handle, int32_t show) {
    return guarded([&]() -> mc_status_t {
        if (!handle) return MC_ERR_NULL_HANDLE;
        return handle->impl.set_show_stats(show != 0);
    });
}

MC_API mc_status_t mc_ui_set_show_add_modal(mc_player_t handle, int32_t show) {
    return guarded([&]() -> mc_status_t {
        if (!handle) return MC_ERR_NULL_HANDLE;
        return handle->impl.set_show_add_modal(show != 0);
    });
}

MC_API const char* mc_status_string(mc_status_t status) {
    return mcp::pal::status_to_string(status);
}

}  // extern "C"
