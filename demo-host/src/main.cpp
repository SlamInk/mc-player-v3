/*
 * mc-player demo host — Win32 native 单窗口。
 *
 * v1 联调用途：
 *   - 验证 RTSP 信令握手 + RTP 接收 + H.264 Depack 端到端
 *   - H.264 Annex-B 落盘到 mc-player-dump.h264，供 ffplay / ffmpeg 验证
 *   - 控制台实时打印 state 转换 / 包计数 / AU 计数
 *   - _CrtSetDbgFlag 进程退出报告 CRT 内存泄露
 *
 * 用法：
 *   mc-player.exe                # 使用内置默认 RTSP URL
 *   mc-player.exe rtsp://...     # 显式指定 URL
 *
 * 默认 URL 通过 MCP_DEFAULT_RTSP_URL 环境变量也可覆盖。
 */

#include <Windows.h>
#include <share.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "mc-player/mc_player.h"

#ifdef _DEBUG
#  include <crtdbg.h>
#endif

namespace {

constexpr wchar_t kClassName[]      = L"mc_player_demo_window";
constexpr wchar_t kTitle[]          = L"mc-player";
constexpr int     kInitWidth        = 1280;
constexpr int     kInitHeight       = 720;

// v1 默认 RTSP URL — 用户提供的测试摄像头。可通过 argv / 环境变量 MCP_DEFAULT_RTSP_URL 覆盖。
constexpr char    kBuiltinDefaultUrl[] =
    "rtsp://admin:qwerasdf1@192.168.1.110:554/h264/ch1/main/av_stream";

std::atomic<mc_state_t> g_last_state{MC_STATE_IDLE};
FILE*                   g_log_file = nullptr;
mc_player_t             g_player   = nullptr;     // 静态全局，让 WndProc WM_TIMER 能拿到

constexpr UINT_PTR      kUiTickTimerId = 1;
constexpr UINT          kUiTickIntervalMs = 16;    // 60Hz UI tick

void log_line(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (static_cast<std::size_t>(n) >= sizeof(buf)) n = static_cast<int>(sizeof(buf) - 1);
    std::printf("%.*s\n", n, buf);
    std::fflush(stdout);
    if (g_log_file) {
        std::fprintf(g_log_file, "%.*s\n", n, buf);
        std::fflush(g_log_file);
    }
}

const char* state_str(mc_state_t s) noexcept {
    switch (s) {
        case MC_STATE_IDLE:       return "IDLE";
        case MC_STATE_CONNECTING: return "CONNECTING";
        case MC_STATE_PLAYING:    return "PLAYING";
        case MC_STATE_RECOVERING: return "RECOVERING";
        case MC_STATE_FROZEN:     return "FROZEN";
        case MC_STATE_ERROR:      return "ERROR";
        case MC_STATE_CLOSED:     return "CLOSED";
    }
    return "?";
}

const char* event_str(mc_event_type_t t) noexcept {
    switch (t) {
        case MC_EVENT_STATE_CHANGED:    return "STATE_CHANGED";
        case MC_EVENT_STREAM_INFO:      return "STREAM_INFO";
        case MC_EVENT_FIRST_FRAME:      return "FIRST_FRAME";
        case MC_EVENT_DEVICE_LOST:      return "DEVICE_LOST";
        case MC_EVENT_DEVICE_RECOVERED: return "DEVICE_RECOVERED";
        case MC_EVENT_ADAPTER_SWITCHED: return "ADAPTER_SWITCHED";
        case MC_EVENT_NETWORK_DOWN:     return "NETWORK_DOWN";
        case MC_EVENT_NETWORK_UP:       return "NETWORK_UP";
        case MC_EVENT_FREEZE:           return "FREEZE";
        case MC_EVENT_RESUME:           return "RESUME";
        case MC_EVENT_ERROR:            return "ERROR";
    }
    return "?";
}

void on_event(void* /*user*/, const mc_event_t* evt) {
    if (!evt) return;
    if (evt->type == MC_EVENT_STATE_CHANGED) {
        g_last_state.store(evt->new_state, std::memory_order_release);
        log_line("[mc-player] event=%s  %s -> %s",
                 event_str(evt->type),
                 state_str(evt->prev_state), state_str(evt->new_state));
    } else if (evt->type == MC_EVENT_ERROR) {
        log_line("[mc-player] event=ERROR code=%d (%s) msg=%s",
                 static_cast<int>(evt->error_code),
                 mc_status_string(evt->error_code),
                 evt->error_message ? evt->error_message : "");
    } else if (evt->type == MC_EVENT_STREAM_INFO && evt->stream_info) {
        log_line("[mc-player] event=STREAM_INFO video_codec=%d audio_codec=%d audio_sr=%u channels=%u",
                 static_cast<int>(evt->stream_info->video_codec),
                 static_cast<int>(evt->stream_info->audio_codec),
                 evt->stream_info->audio_sample_rate_hz,
                 evt->stream_info->audio_channels);

        const char* dec_label = "unknown";
        switch (evt->stream_info->video_decoder_kind) {
            case MC_DECODER_NONE:         dec_label = "none";                   break;
            case MC_DECODER_MFT_HARDWARE: dec_label = "MFT 硬解";                break;
            case MC_DECODER_MFT_SOFTWARE: dec_label = "MFT 软解 (OS software)"; break;
            case MC_DECODER_LIBCODEC:     dec_label = "mc-libcodec 软解";        break;
        }
        const char* gpu_label = "unknown";
        switch (evt->stream_info->gpu_kind) {
            case MC_GPU_KIND_UNKNOWN:  gpu_label = "未知 GPU";  break;
            case MC_GPU_KIND_IGPU:     gpu_label = "集显";       break;
            case MC_GPU_KIND_DGPU:     gpu_label = "独显";       break;
            case MC_GPU_KIND_SOFTWARE: gpu_label = "软件 adapter (WARP)"; break;
        }
        const char* desc = evt->stream_info->gpu_description;
        log_line("[mc-player] decoder=%s  gpu=%s  adapter=\"%s\"",
                 dec_label, gpu_label, (desc && *desc) ? desc : "(unknown)");
    } else {
        log_line("[mc-player] event=%s", event_str(evt->type));
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_TIMER:
            if (w == kUiTickTimerId && g_player) {
                mc_tick_ui(g_player);
            }
            return 0;
        case WM_CLOSE:   ::DestroyWindow(hwnd); return 0;
        case WM_DESTROY: ::PostQuitMessage(0);  return 0;
        default:         return ::DefWindowProcW(hwnd, msg, w, l);
    }
}

std::string utf16_to_utf8(const wchar_t* w) {
    if (!w) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

void attach_console() {
    if (!::AttachConsole(ATTACH_PARENT_PROCESS)) {
        ::AllocConsole();
    }
    FILE* fout = nullptr; FILE* ferr = nullptr;
    freopen_s(&fout, "CONOUT$", "w", stdout);
    freopen_s(&ferr, "CONOUT$", "w", stderr);
    std::printf("\n[mc-player] console attached, pid=%lu\n", ::GetCurrentProcessId());
    std::fflush(stdout);
}

std::string resolve_url(const std::string& argv_url) {
    if (!argv_url.empty()) return argv_url;
    if (const char* env = std::getenv("MCP_DEFAULT_RTSP_URL"); env && *env) {
        return env;
    }
    return kBuiltinDefaultUrl;
}

void run_stats_pump(mc_player_t player, std::atomic<bool>& stop) {
    auto last_packets  = uint64_t{0};
    auto last_aus      = uint64_t{0};
    auto last_presents = uint64_t{0};
    while (!stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (stop.load(std::memory_order_acquire)) break;

        mc_stats_t s{};
        s.struct_size    = sizeof(s);
        s.struct_version = MC_STATS_VERSION;
        if (mc_get_stats(player, &s) == MC_OK) {
            const uint64_t pps  = s.rtp_packets_received - last_packets;
            const uint64_t aups = s.video_frames_decoded - last_aus;
            const uint64_t fps  = s.present_count       - last_presents;
            last_packets  = s.rtp_packets_received;
            last_aus      = s.video_frames_decoded;
            last_presents = s.present_count;
            log_line("[mc-player] state=%s  rtp_pkts=%llu (+%llu/s)  aus=%llu (+%llu/s)  presents=%llu (+%llu/s)",
                     state_str(g_last_state.load(std::memory_order_acquire)),
                     static_cast<unsigned long long>(s.rtp_packets_received),
                     static_cast<unsigned long long>(pps),
                     static_cast<unsigned long long>(s.video_frames_decoded),
                     static_cast<unsigned long long>(aups),
                     static_cast<unsigned long long>(s.present_count),
                     static_cast<unsigned long long>(fps));
            if (s.gate_poisoned || s.gate_poison_drops > 0) {
                log_line("[mc-player] gate poisoned=%u source=%d enter_count=%llu poison_drops=%llu",
                         static_cast<unsigned>(s.gate_poisoned),
                         static_cast<int>(s.gate_last_poison_source),
                         static_cast<unsigned long long>(s.gate_poison_enter_count),
                         static_cast<unsigned long long>(s.gate_poison_drops));
            }
        }
    }
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR cmd_line, int) {
#ifdef _DEBUG
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    attach_console();
    // 单次 truncate，再以 append 模式与 mc_global_init 内部 sink 共写同一文件。
    // append 模式在 Windows 上对单次 fwrite 隐式 SetFilePointer→end，避免两 writer 行交错。
    if (FILE* tmp = ::_fsopen("mc-player.log", "w", _SH_DENYNO)) std::fclose(tmp);
    g_log_file = ::_fsopen("mc-player.log", "a", _SH_DENYNO);

    const std::string argv_url = utf16_to_utf8(cmd_line);
    const std::string url      = resolve_url(argv_url);
    log_line("[mc-player] url=%s", url.c_str());

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    if (!::RegisterClassExW(&wc)) return 2;

    HWND hwnd = ::CreateWindowExW(0, kClassName, kTitle,
                                   WS_OVERLAPPEDWINDOW,
                                   CW_USEDEFAULT, CW_USEDEFAULT,
                                   kInitWidth, kInitHeight,
                                   nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 3;

    mc_init_options_t init{};
    init.struct_size        = sizeof(init);
    init.struct_version     = MC_INIT_OPTIONS_VERSION;
    init.enable_etw_tracing = 1;
    init.log_to_debugger    = 1;
    init.log_file_path      = "mc-player.log";    // 与 demo log_line() 共写同一文件（追加模式）
    if (mc_status_t s = mc_global_init(&init); s != MC_OK) {
        log_line("[mc-player] mc_global_init failed: %s", mc_status_string(s));
        ::DestroyWindow(hwnd);
        return 4;
    }

    mc_player_t player = nullptr;
    if (mc_status_t s = mc_create(&player); s != MC_OK) {
        log_line("[mc-player] mc_create failed: %s", mc_status_string(s));
        mc_global_shutdown();
        return 5;
    }
    g_player = player;

    mc_set_event_callback(player, on_event, nullptr);
    mc_set_render_target(player, hwnd);

    mc_open_options_t opts{};
    opts.struct_size            = sizeof(opts);
    opts.struct_version         = MC_OPEN_OPTIONS_VERSION;
    opts.url                    = url.c_str();
    opts.protocol_hint          = MC_PROTOCOL_RTSP_TCP;     // 强制 TCP interleaved，海康 UDP 不稳定推 audio
    opts.render_profile_hint    = MC_RENDER_PROFILE_AUTO;
    opts.allow_software_decode  = 1;
    opts.allow_tcp_fallback     = 1;
    if (mc_status_t s = mc_open(player, &opts); s != MC_OK) {
        log_line("[mc-player] mc_open failed: %s", mc_status_string(s));
    }

    std::atomic<bool> stop_pump{false};
    std::thread stats_thread([&] { run_stats_pump(player, stop_pump); });

    ::ShowWindow(hwnd, SW_SHOW);
    ::UpdateWindow(hwnd);

    // 60Hz UI tick — 驱动 UI overlay 动画时间基（cBreathe / cBlink / connecting elapsed 等）。
    ::SetTimer(hwnd, kUiTickTimerId, kUiTickIntervalMs, nullptr);

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    ::KillTimer(hwnd, kUiTickTimerId);
    stop_pump.store(true, std::memory_order_release);
    if (stats_thread.joinable()) stats_thread.join();

    g_player = nullptr;
    mc_close(player);
    mc_destroy(player);
    mc_global_shutdown();

    log_line("[mc-player] exit");
    if (g_log_file) std::fclose(g_log_file);
    return static_cast<int>(msg.wParam);
}
