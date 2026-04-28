/*
 * mc-player public C ABI — main entry.
 *
 * 设计原则：
 *   - 纯 C 链接，no exceptions across boundary。
 *   - 不透明句柄 mc_player_t，App 不持有内部布局。
 *   - 所有 struct 第一字段 size_t struct_size，第二字段 uint32_t struct_version。
 *   - 一个 session = 一条流。多窗口 = 多 session。
 *   - 配置驱动：v1 RTSP-only 也通过 mc_open_options_t 决定，无任何硬编码 URL/端口/超时。
 *
 * 线程：
 *   mc_create / mc_destroy 只能由 App 主线程调用。
 *   mc_open / mc_close / mc_set_render_target 可由任何线程，但同一 session 不可重入。
 *   事件回调由库内部线程触发（详见 §3.3 线程视图）；App 须自行 marshal 至 UI 线程。
 */

#ifndef MC_PLAYER_H_
#define MC_PLAYER_H_

#include "mc_player_types.h"
#include "mc_player_stats.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  if defined(MCP_BUILD_LIBRARY) && defined(MCP_BUILD_SHARED)
#    define MC_API __declspec(dllexport)
#  elif defined(MCP_USE_SHARED)
#    define MC_API __declspec(dllimport)
#  else
#    define MC_API
#  endif
#else
#  define MC_API
#endif

/* 版本号（语义化） */
#define MC_PLAYER_VERSION_MAJOR  0
#define MC_PLAYER_VERSION_MINOR  1
#define MC_PLAYER_VERSION_PATCH  0

/* struct_version 当前默认值；按字段追加单调递增。 */
#define MC_OPEN_OPTIONS_VERSION  1
#define MC_INIT_OPTIONS_VERSION  2
#define MC_STREAM_INFO_VERSION   2     /* +gpu_kind / gpu_description */
#define MC_EVENT_VERSION         1
#define MC_STATS_VERSION         2     /* +gate_poisoned/last_source/enter_count/poison_drops */

/* ──────────────────────────────────────────────────────────────
 * 全局初始化（进程级一次性 — MFStartup / WSAStartup / COM）
 * 每次 mc_global_init 必有匹配的 mc_global_shutdown。
 * 嵌套调用允许；按引用计数生效。
 * ─────────────────────────────────────────────────────────────*/
typedef struct mc_init_options_s {
    size_t              struct_size;
    uint32_t            struct_version;

    int32_t             enable_etw_tracing;     /* 0/1 */
    int32_t             log_to_debugger;        /* 0/1 — OutputDebugString 镜像 */
    const char*         log_ringbuf_dir;        /* nullable，若非空则 ringbuf 落盘目录 */

    /* version >= 2 起：单文件追加 sink。nullable。便于联调时把内部诊断写到 host 自管 log。 */
    const char*         log_file_path;
} mc_init_options_t;

MC_API mc_status_t mc_global_init(const mc_init_options_t* options);
MC_API mc_status_t mc_global_shutdown(void);

/* ──────────────────────────────────────────────────────────────
 * 会话生命周期
 * ─────────────────────────────────────────────────────────────*/
MC_API mc_status_t mc_create(mc_player_t* out_handle);
MC_API mc_status_t mc_destroy(mc_player_t handle);

typedef struct mc_open_options_s {
    size_t              struct_size;
    uint32_t            struct_version;

    /* 必填：媒体源 URL（rtsp://… 或 rtsps://…，未来 whep://…）。
     * 协议由 scheme 解析，配合 protocol_hint 决定 transport。 */
    const char*         url;

    /* 可选：协议提示。AUTO = 由 URL 决定 + 自动 UDP→TCP 降级。 */
    mc_protocol_t       protocol_hint;

    /* 可选认证（RTSP Digest / Basic）；nullptr 时按 URL 内嵌 user:pass 解析。 */
    const char*         username;
    const char*         password;

    /* 渲染 profile 提示。AUTO = 探测。其他值跳过探测直接用。 */
    mc_render_profile_t render_profile_hint;

    /* 显式指定 GPU LUID。0 = 智能选择（HWND 跟随）。
     * 用 LowPart/HighPart 拼出 LUID，避免引入 windows.h 的 LUID 类型到公开 ABI。 */
    uint32_t            adapter_luid_low;
    int32_t             adapter_luid_high;

    /* 色彩元数据用户覆盖（ADD §5.12 第三级）。AUTO = 不覆盖。 */
    mc_color_primaries_t override_color_primaries;
    mc_color_range_t    override_color_range;
    mc_color_matrix_t   override_color_matrix;

    /* 网络层超时（毫秒）。0 = 使用库默认值（命名常量定义在内部）。 */
    uint32_t            connect_timeout_ms;
    uint32_t            read_timeout_ms;
    uint32_t            keepalive_interval_ms;

    /* Jitter buffer 上下界（毫秒）。0 = 使用库默认值。 */
    uint32_t            min_target_delay_ms;
    uint32_t            max_target_delay_ms;

    /* 是否允许 mc-libcodec 软解兜底（默认 1=允许）。 */
    int32_t             allow_software_decode;

    /* 是否在 RTSP UDP 失败时降级 interleaved TCP（默认 1）。 */
    int32_t             allow_tcp_fallback;
} mc_open_options_t;

MC_API mc_status_t mc_open(mc_player_t handle, const mc_open_options_t* options);
MC_API mc_status_t mc_close(mc_player_t handle);

/* ──────────────────────────────────────────────────────────────
 * 渲染目标 — App 指定承载视频的 HWND
 * 必须在 mc_open 之前或 mc_open 之后立即调用；运行期切换会触发 soft adapter switch。
 * ─────────────────────────────────────────────────────────────*/
#if defined(_WIN32)
MC_API mc_status_t mc_set_render_target(mc_player_t handle, HWND hwnd);
#endif

/* ──────────────────────────────────────────────────────────────
 * 事件 / 状态查询
 * ─────────────────────────────────────────────────────────────*/
MC_API mc_status_t mc_set_event_callback(mc_player_t handle,
                                          mc_event_callback_fn callback,
                                          void* user_data);

MC_API mc_status_t mc_get_state(mc_player_t handle, mc_state_t* out_state);

MC_API mc_status_t mc_get_stats(mc_player_t handle, mc_stats_t* inout_stats);

MC_API mc_status_t mc_get_stream_info(mc_player_t handle, mc_stream_info_t* inout_info);

/* ──────────────────────────────────────────────────────────────
 * UI tick — 由 host（demo-host / 应用主消息泵）以 60Hz（或 30Hz 节能档）调用，
 * 用于推进 UI overlay 动画时间基（cBreathe / cBlink / cLive / connecting elapsed
 * / scan glow 等）。idle/error/connecting 等无视频帧 stage 由 tick 驱动 Present；
 * playing stage 由视频帧自然 redraw 驱动，tick 只更新动画相位。
 * 推荐间隔 16ms。
 * ─────────────────────────────────────────────────────────────*/
MC_API mc_status_t mc_tick_ui(mc_player_t handle);

/* ──────────────────────────────────────────────────────────────
 * UI 瞬态状态（与 mc_state 正交）
 *   - show_stats：playing stage 是否叠加右上 stats drawer
 *   - show_add_modal：是否显示「添加流」模态遮罩
 * ─────────────────────────────────────────────────────────────*/
MC_API mc_status_t mc_ui_set_show_stats(mc_player_t handle, int32_t show);
MC_API mc_status_t mc_ui_set_show_add_modal(mc_player_t handle, int32_t show);

/* ──────────────────────────────────────────────────────────────
 * 错误码 → 字符串（仅用于日志/调试，库内静态字符串，不需释放）
 * ─────────────────────────────────────────────────────────────*/
MC_API const char* mc_status_string(mc_status_t status);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* MC_PLAYER_H_ */
