#include "controller/controller.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3d11_4.h>          // ID3D11Multithread（同 device 上的并发保护开关）
#include <dxgi1_6.h>
#include <mfapi.h>            // MFTEnumEx / MFT_CATEGORY_VIDEO_DECODER (Phase 1 probe_mft_video_async_hardware)
#include <mfobjects.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "controller/adapter_picker.h"
#include "media/audio_render_wasapi.h"
#include "hdcm/detector.h"
#include "media/codec_amf.h"
#include "media/codec_dxva_video.h"
#include "media/codec_nvdec.h"
#include "media/codec_onevpl.h"
#include "media/codec_g711.h"
#include "media/codec_libcodec.h"
#include "media/codec_mft_audio.h"
#include "media/codec_mft_video.h"
#include "media/color_meta.h"
#include "media/depack_aac.h"
#include "media/depack_h264.h"
#include "media/depack_h265.h"
#include "media/frame_validity_gate.h"
#include "media/jitter_buffer_video.h"
#include "media/render_d3d11.h"
#include "media/ui_overlay.h"
#include "pal/clock.h"
#include "pal/dxgi_caps_probe.h"
#include "pal/log.h"
#include "pal/metric.h"
#include "transport/sdp_parser.h"
#include "transport/transport_session.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace mcp::controller {

namespace {

constexpr uint32_t kDefaultConnectTimeoutMs = 5000;
constexpr uint32_t kDefaultReadTimeoutMs    = 5000;
constexpr uint32_t kDefaultKeepaliveMs      = 25'000;

mc_video_codec_t map_video_codec(const std::string& name) noexcept {
    if (name == "H264") return MC_VIDEO_CODEC_H264;
    if (name == "H265" || name == "HEVC") return MC_VIDEO_CODEC_H265;
    if (name == "AV1")  return MC_VIDEO_CODEC_AV1;
    return MC_VIDEO_CODEC_UNKNOWN;
}

/* iGPU vs dGPU 启发式：
 *   - is_software → SOFTWARE（WARP / Basic Render Driver）
 *   - DedicatedVideoMemory >= 1 GB → DGPU
 *     · NVIDIA dGPU 自 GT 1030 起 ≥2 GB；AMD dGPU ≥2 GB；Intel Arc ≥6 GB
 *   - 否则 → IGPU
 *     · Intel UHD/Iris UMA 报告 ~128 MB 专用；AMD APU Vega/Radeon Graphics 类似
 * 对极少数低端 dGPU（<1 GB VRAM）会误判，但现网占比可忽略。 */
mc_gpu_kind_t classify_gpu(const mcp::pal::AdapterCaps& a) noexcept {
    if (a.is_software) return MC_GPU_KIND_SOFTWARE;
    constexpr uint64_t kDgpuVramThreshold = 1024ull * 1024ull * 1024ull;  // 1 GB
    return a.dedicated_video_memory >= kDgpuVramThreshold ? MC_GPU_KIND_DGPU : MC_GPU_KIND_IGPU;
}

void copy_adapter_description(char (&dst)[96], const std::wstring& src) noexcept {
    if (src.empty()) { dst[0] = '\0'; return; }
    int n = ::WideCharToMultiByte(CP_UTF8, 0, src.c_str(), -1,
                                    dst, static_cast<int>(sizeof(dst)),
                                    nullptr, nullptr);
    if (n <= 0) { dst[0] = '\0'; return; }
    dst[sizeof(dst) - 1] = '\0';   // 保险：截断时强制 null 终止
}

std::wstring utf8_to_wide(const char* s) noexcept {
    if (!s || !*s) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

mcp::media::UiStage map_state_to_ui_stage(mc_state_t s) noexcept {
    switch (s) {
        case MC_STATE_IDLE:
        case MC_STATE_CLOSED:    return mcp::media::UiStage::empty;
        case MC_STATE_CONNECTING:
        case MC_STATE_RECOVERING:return mcp::media::UiStage::connecting;
        case MC_STATE_PLAYING:
        case MC_STATE_FROZEN:    return mcp::media::UiStage::playing;
        case MC_STATE_ERROR:     return mcp::media::UiStage::error;
    }
    return mcp::media::UiStage::empty;
}

// ADR-015 + plan Phase 1：档 3 MFT hardware async probe 结果 → 区分 tier_skip_reason。
// codec_mft_video::start 在 sync only 时也返 MC_ERR_NO_HARDWARE,无法从 status 区分原因;
// controller 在 codec_mft_video::start 失败前先用此 helper 探测真实情况以填准确 reason。
enum class MftProbeReason {
    hardware_async_available,   // 至少一个 hw_url=1 && async=1 的 MFT
    sync_software_available,    // 仅 sync software MFT(hw_url=0 && async=0),IoT LTSC 兜底可用
    no_mft_registered,          // MediaPlayback feature 缺失/未启用，MFT category 未注册
    enum_failed,                // MFTEnumEx 返回 FAILED
};

[[nodiscard]] MftProbeReason probe_mft_video_async_hardware(mc_video_codec_t codec) noexcept {
    GUID subtype = GUID_NULL;
    switch (codec) {
        case MC_VIDEO_CODEC_H264: subtype = MFVideoFormat_H264; break;
        case MC_VIDEO_CODEC_H265: subtype = MFVideoFormat_HEVC; break;
        case MC_VIDEO_CODEC_AV1:  subtype = MFVideoFormat_AV1;  break;
        default: return MftProbeReason::no_mft_registered;
    }
    MFT_REGISTER_TYPE_INFO in_info{MFMediaType_Video, subtype};

    // 先试 HARDWARE | ASYNCMFT — 命中即可用。
    IMFActivate** activates = nullptr;
    UINT32        n         = 0;
    HRESULT       hr        = ::MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                                            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT |
                                              MFT_ENUM_FLAG_SORTANDFILTER,
                                            &in_info, nullptr, &activates, &n);
    if (SUCCEEDED(hr) && n > 0) {
        for (UINT32 i = 0; i < n; ++i) activates[i]->Release();
        ::CoTaskMemFree(activates);
        return MftProbeReason::hardware_async_available;
    }
    if (activates) ::CoTaskMemFree(activates);

    // 没有硬件 async — 看 ALL 列表分辨"仅 sync software" vs "无注册"。
    activates = nullptr;
    n         = 0;
    hr        = ::MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_ALL,
                              &in_info, nullptr, &activates, &n);
    if (FAILED(hr)) {
        if (activates) ::CoTaskMemFree(activates);
        return MftProbeReason::enum_failed;
    }
    const bool has_any = n > 0;
    for (UINT32 i = 0; i < n; ++i) activates[i]->Release();
    if (activates) ::CoTaskMemFree(activates);
    return has_any ? MftProbeReason::sync_software_available
                    : MftProbeReason::no_mft_registered;
}

[[nodiscard]] const char* mft_probe_reason_str(MftProbeReason r) noexcept {
    switch (r) {
        case MftProbeReason::hardware_async_available: return "hardware_async_available";
        case MftProbeReason::sync_software_available:  return "sync_software_available";
        case MftProbeReason::no_mft_registered:        return "no_mft_registered";
        case MftProbeReason::enum_failed:              return "enum_failed";
    }
    return "unknown";
}

const transport::SdpMedia* find_media(const transport::SdpSession& sdp,
                                       transport::SdpMedia::Kind k) noexcept {
    for (const auto& m : sdp.media) {
        if (m.kind == k) return &m;
    }
    return nullptr;
}

}  // namespace

struct Controller::Impl {
    std::mutex                                 mu;
    EventSink                                  sink;
    std::atomic<mc_state_t>                    state{MC_STATE_IDLE};

    HWND                                       render_hwnd{nullptr};
    mc_open_options_t                          options{};

    // 进程级 COM/MF/WSA runtime 由 mc_global_init 持有；本类不重复 acquire。
    pal::DxgiCapsProbe                         caps_probe;

    ComPtr<ID3D11Device>                       d3d_device;

    std::unique_ptr<transport::TransportSession>  transport;
    std::unique_ptr<media::JitterBufferVideo>     jitter_video;     // RTP reorder + NACK
    std::unique_ptr<media::DepackH264>            depack_h264;
    std::unique_ptr<media::DepackH265>            depack_h265;
    std::unique_ptr<media::CodecMftVideo>         codec_video;
    std::unique_ptr<media::CodecDxvaVideo>        codec_dxva;
    std::unique_ptr<media::CodecNvdec>            codec_nvdec;
    std::unique_ptr<media::CodecOneVPL>           codec_onevpl;
    std::unique_ptr<media::CodecAmf>              codec_amf;
    std::unique_ptr<media::CodecLibcodecVideo>    codec_libcodec;
    std::unique_ptr<media::FrameValidityGate>     gate;
    std::unique_ptr<media::RenderD3d11>           render;

    // sync software MFT silent fail 降档:tick_ui 周期 poll codec_video 状态,
    // 检测到 silent_fail_confirmed 后只触发一次降档(避免 race 重复 stop/start)。
    bool                                          tier3_demoted_to_tier4 = false;

    // 音频管线（ADD §5.8 / §5.11）：RTP audio → depack → MFT decode → WASAPI 渲染。
    std::unique_ptr<media::DepackAac>             depack_aac;
    std::unique_ptr<media::CodecMftAudio>         codec_audio;
    std::unique_ptr<media::AudioRenderWasapi>     audio_render;
    mc_audio_codec_t                              audio_codec       = MC_AUDIO_CODEC_UNKNOWN;
    uint32_t                                      audio_clock_rate  = 0;     // RTP TS 单位
    uint32_t                                      audio_channels    = 0;

    // 调试辅助：可选 dump（默认关闭，置 1 重启会写到磁盘）。
    bool                                       dump_enabled = false;
    FILE*                                      dump_file    = nullptr;
    bool                                       dump_armed   = false;

    std::atomic<uint64_t>                      rtp_packets{0};
    std::atomic<uint64_t>                      au_count{0};
    std::atomic<uint64_t>                      idr_count{0};

    // RTCP 上行:本端 sender SSRC(每会话随机一次) + 视频远端 SSRC(首个视频 RTP 学到)。
    // first_pli_sent 是首帧专用的 atomic_flag(test_and_set 一次性触发);
    // SEQ_GAP 路径不复用它,改用限频 last_video_pli_ns_ + video_remote_ssrc 重发 PLI。
    uint32_t                                   local_sender_ssrc = 0;
    std::atomic_flag                           first_pli_sent = ATOMIC_FLAG_INIT;
    // 视频远端 SSRC,首个 video RTP 学到后保存供 SEQ_GAP 后续重发 PLI 复用。
    std::atomic<uint32_t>                      video_remote_ssrc{0};
    // 上次 PLI 发送的 QPC ns,RFC 4585 §6.3.1 immediate feedback minimum interval
    // 用于限频:连续 SEQ_GAP 风暴下不要每次都发 PLI(否则反向 DDoS 摄像头),
    // 200 ms 是 dither_max 的实务下限,且 << GOP 周期(几秒),不会拖慢恢复。
    std::atomic<int64_t>                       last_video_pli_ns{0};
    static constexpr int64_t                   kPliMinIntervalNs = 200'000'000;  // 200 ms

    // Phase 2：RTP 视频 seq 跟踪 + 16-bit wrap-aware gap 检测。gap>0 时调
    // depack->mark_reference_lost + gate->mark_poisoned(SEQ_GAP)，让 §5.13 Frame
    // Validity Gate 显式上报 mc.gate.tainted_source.seq_gap counter（plan §2.0 6 类细分）。
    bool                                       video_seq_inited  = false;
    uint16_t                                   video_seq_last    = 0;

    mc_video_codec_t                           video_codec   = MC_VIDEO_CODEC_UNKNOWN;
    mc_decoder_kind_t                          decoder_kind  = MC_DECODER_NONE;
    AdapterPick                                adapter_pick{};

    // ADD §5.6.4 / §5.12：B-Frame Policy + Color VUI 三级兜底的输出。
    // on_stream_ready 内根据 SDP profile-level-id + depack 解出的 SPS 综合判定，
    // 在 start_decode_pipeline 创建 codec 时灌入。
    bool                                       decoded_no_b_frames = true;     // 默认无 B 帧
    media::ResolvedColor                       resolved_color {
        MC_COLOR_PRIMARIES_BT709,
        MC_COLOR_RANGE_LIMITED,
        MC_COLOR_MATRIX_BT709,
    };

    // 音频管线在独立线程异步启动 —— WASAPI IAudioClient::Initialize 同步耗时 100~200 ms，
    // 若在 transport on_stream_ready 调用线程上阻塞，会推迟 depack/codec 创建及 PLI 触发，
    // 视频首帧延时直接增加这一段。线程在 close() 阶段一无锁 join，避免与 close_unlocked 竞争。
    std::thread                                audio_start_thread;

    void emit_state_change(mc_state_t prev, mc_state_t now) noexcept {
        if (prev == now) return;
        // mc_state → UiStage 自动映射（render 在 start_decode_pipeline 后才存在；
        // 第一次切 CONNECTING 时 render 还没创建，这里 noop，后续 render 启动时
        // 会读 state 同步当前 stage）。
        if (render && render->ui_overlay()) {
            render->ui_overlay()->set_stage(map_state_to_ui_stage(now));
        }
        Event e;
        e.type       = MC_EVENT_STATE_CHANGED;
        e.prev_state = prev;
        e.new_state  = now;
        if (sink) sink(e);
    }

    void emit_error(mc_status_t code, std::string msg) noexcept {
        const mc_state_t prev = state.exchange(MC_STATE_ERROR, std::memory_order_acq_rel);
        // 错误信息推到 UI（CONNECTION_TIMEOUT / no SDP / device lost...）
        if (render && render->ui_overlay()) {
            render->ui_overlay()->set_stage(mcp::media::UiStage::error);
            render->ui_overlay()->set_error(
                utf8_to_wide(::mc_status_string(code)),
                utf8_to_wide(msg.c_str()),
                utf8_to_wide(options.url ? options.url : ""));
        }
        Event e;
        e.type          = MC_EVENT_ERROR;
        e.new_state     = MC_STATE_ERROR;
        e.prev_state    = prev;
        e.error_code    = code;
        e.error_message = std::move(msg);
        if (sink) sink(e);
    }

    void open_dump_if_enabled(const char* filename) noexcept {
        if (!dump_enabled || dump_file) return;
        if (::fopen_s(&dump_file, filename, "wb") == 0 && dump_file) {
            MCP_LOGF(pal::LogLevel::info, "Controller: dumping Annex-B to %s", filename);
        }
    }

    void close_dump_file() noexcept {
        if (dump_file) {
            std::fflush(dump_file);
            std::fclose(dump_file);
            dump_file = nullptr;
        }
    }

    mc_status_t create_d3d_device() noexcept {
        ComPtr<IDXGIFactory6> factory = caps_probe.factory();
        if (!factory) return MC_ERR_INVALID_STATE;

        ComPtr<IDXGIAdapter1> picked;
        if (adapter_pick.luid.LowPart != 0 || adapter_pick.luid.HighPart != 0) {
            ComPtr<IDXGIAdapter> a;
            if (SUCCEEDED(factory->EnumAdapterByLuid(adapter_pick.luid, IID_PPV_ARGS(&a)))) {
                a.As(&picked);
            }
        }
        // 兜底：取第一个非软件 adapter。
        if (!picked) {
            UINT i = 0;
            ComPtr<IDXGIAdapter1> it;
            while (factory->EnumAdapters1(i++, &it) != DXGI_ERROR_NOT_FOUND) {
                DXGI_ADAPTER_DESC1 d{};
                it->GetDesc1(&d);
                if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    picked = it;
                    break;
                }
                it.Reset();
            }
        }
        if (!picked) return MC_ERR_NO_HARDWARE;

        const D3D_FEATURE_LEVEL fls[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        };
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
        // Debug layer 仅在已安装 Graphics Tools 时可用；失败兜底到无 layer。
        ComPtr<ID3D11Device> dbg;
        if (SUCCEEDED(::D3D11CreateDevice(picked.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                            flags | D3D11_CREATE_DEVICE_DEBUG,
                                            fls, _countof(fls),
                                            D3D11_SDK_VERSION, &dbg, nullptr, nullptr))) {
            d3d_device = std::move(dbg);
            return MC_OK;
        }
#endif
        D3D_FEATURE_LEVEL got{};
        ComPtr<ID3D11DeviceContext> ctx;
        HRESULT hr = ::D3D11CreateDevice(picked.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                          flags, fls, _countof(fls),
                                          D3D11_SDK_VERSION, &d3d_device, &got, &ctx);
        if (FAILED(hr)) {
            MCP_LOGF(pal::LogLevel::error,
                     "Controller: D3D11CreateDevice failed hr=0x%08lX", hr);
            return MC_ERR_INTERNAL;
        }
        enable_multithread_protection();
        apply_low_latency_dxgi();
        return MC_OK;
    }

    // Step 2: device-level DXGI present queue 深度。与 swap chain 级
    // IDXGISwapChain2::SetMaximumFrameLatency(1)（render_swap_chain.cpp:90）独立 ——
    // DXGI runtime 默认 device 级 max-latency=3，未限会让 driver 内部 present queue 多
    // 积 1-2 帧。两层都设 1 才能让端到端 present 延时降到 ≈1 frame period。失败仅记
    // 日志不阻断启动。
    void apply_low_latency_dxgi() noexcept {
        if (!d3d_device) return;
        ComPtr<IDXGIDevice1> dxgi_dev;
        if (FAILED(d3d_device.As(&dxgi_dev)) || !dxgi_dev) {
            MCP_LOG_WARN("Controller: IDXGIDevice1 query failed; device-level frame latency skipped");
            return;
        }
        HRESULT hr = dxgi_dev->SetMaximumFrameLatency(1);
        MCP_LOGF(SUCCEEDED(hr) ? pal::LogLevel::info : pal::LogLevel::warn,
                 "Controller: device-level SetMaximumFrameLatency(1) hr=0x%08lX", hr);
    }

    // CLAUDE.md 硬约束："MFT 与 renderer 共享同一 ID3D11Device 并启用多线程保护"。
    // codec_dxva（transport 线程驱动）与 render（同线程 + tick_ui 主线程）会并发使用同一
    // immediate context。AMD/NV 部分驱动在并发时进入 race window 导致进程崩溃，必须设。
    void enable_multithread_protection() noexcept {
        if (!d3d_device) return;
        ComPtr<ID3D11DeviceContext> imm;
        d3d_device->GetImmediateContext(&imm);
        if (!imm) return;
        ComPtr<ID3D11Multithread> mt;
        if (SUCCEEDED(imm.As(&mt)) && mt) {
            mt->SetMultithreadProtected(TRUE);
            MCP_LOG_INFO("Controller: D3D11 multi-thread protect enabled");
        } else {
            MCP_LOG_WARN("Controller: ID3D11Multithread query failed; concurrent ctx access may crash");
        }
    }

    // ownership 移交链：depack → on_video_au_* → submit_au_to_decoder → codec.submit。
    // 整条路径无中间 memcpy（vector buffer 直接移交给 codec 的 PendingAu）。
    // arrival_qpc_ns 是端到端延时探针起点（AU first-packet RX 戳），由 codec 透传到 VideoFrame。
    void submit_au_to_decoder(std::vector<uint8_t>&& bytes, int64_t pts_us,
                                int64_t arrival_qpc_ns) noexcept {
        // 性能量度规范 §2.2 mc.stage.decode_alloc_ns：AU → decoder buffer 就绪。
        // ScopedTimer 仅记本函数 dispatch 耗时（depack→codec 移交 + codec.submit 入队）；
        // 真正的 codec 内部 alloc + memcpy 由各 codec 实装（codec_mft_video / codec_dxva /
        // codec_libcodec）在 hot path 内独立 record。Phase 0 仅暴露字段,phase 1+ 各 codec 补全。
        pal::metric::ScopedTimer t{
            pal::metric::Registry::instance().timer("mc.stage.decode_alloc_ns")};
        if (codec_video)         codec_video->submit(std::move(bytes), pts_us, arrival_qpc_ns);
        else if (codec_dxva)     codec_dxva->submit(std::move(bytes), pts_us, arrival_qpc_ns);
        else if (codec_libcodec) codec_libcodec->submit(std::move(bytes), pts_us, arrival_qpc_ns);
    }

    void on_video_au_h264(media::H264AccessUnit&& au) noexcept {
        au_count.fetch_add(1, std::memory_order_relaxed);
        if (au.has_idr) {
            idr_count.fetch_add(1, std::memory_order_relaxed);
            dump_armed = true;
        }
        // dump 必须先于 move：std::move 之后 au.annexb_bytes 被掏空。
        if (dump_armed && dump_file && !au.annexb_bytes.empty()) {
            std::fwrite(au.annexb_bytes.data(), 1, au.annexb_bytes.size(), dump_file);
        }
        submit_au_to_decoder(std::move(au.annexb_bytes), au.pts_us, au.arrival_qpc_ns);
    }

    void on_video_au_h265(media::H265AccessUnit&& au) noexcept {
        au_count.fetch_add(1, std::memory_order_relaxed);
        if (au.has_irap) {
            idr_count.fetch_add(1, std::memory_order_relaxed);
            dump_armed = true;
        }
        if (dump_armed && dump_file && !au.annexb_bytes.empty()) {
            std::fwrite(au.annexb_bytes.data(), 1, au.annexb_bytes.size(), dump_file);
        }
        submit_au_to_decoder(std::move(au.annexb_bytes), au.pts_us, au.arrival_qpc_ns);
    }

    void on_decoded_frame(media::VideoFrame&& f) noexcept {
        // 性能量度规范 §2.1 mc.e2e.client_internal_ns：rx_first_byte → emit。
        // 此处 emit 时点是 codec 出帧 → gate.admit；arrival_qpc_ns 由 codec 透传（depack 层
        // RTP 首包到达戳）。仅 arrival_qpc_ns 有效时 record，避免冷启动期 0 戳污染分位数。
        if (f.arrival_qpc_ns > 0) {
            const int64_t now_ns = pal::Clock::now_ns();
            const int64_t delta  = now_ns - f.arrival_qpc_ns;
            if (delta > 0) {
                pal::metric::Registry::instance()
                    .timer("mc.e2e.client_internal_ns").record(delta);
            }
        }
        // 性能量度规范 §3.3 mc.decode.fps_emit_to_gate counter 等价（fps 是 1Hz 聚合）。
        pal::metric::Registry::instance()
            .counter("mc.decode.frame_emit_to_gate_count").inc();

        if (gate) gate->admit(f);
    }

    void on_admitted(const media::VideoFrame& f) noexcept {
        // 性能量度规范 §2.2 mc.stage.yuv2rgb_ns：dual-bind / CopySub + YUV→RGB shader。
        // 实际 shader dispatch 在 render->on_admitted 内部；此处粗记函数耗时。
        pal::metric::ScopedTimer t{
            pal::metric::Registry::instance().timer("mc.stage.yuv2rgb_ns")};
        if (render) render->on_admitted(f);
    }

    // ADD §5.6.4 B-Frame Policy + §5.12 Color VUI 三级兜底：
    // 在 start_decode_pipeline 创建 codec 之前必须已计算 decoded_no_b_frames + resolved_color。
    //
    // B-frame 判定（VLC 风格："乐观默认 + 运行期检测降级"）：
    //   默认 decoded_no_b_frames = true（开 LowLatencyMode）。仅在 SPS 明示
    //   reorder > 0（H.264 bitstream_restriction=1 + max_num_reorder_frames>0
    //   或 H.265 sps_max_num_reorder_pics>0）时关。
    //
    //   SDP profile-level-id 不再用作判据 —— 现网 zero-latency 编码器即便报 Main/High
    //   profile 也常 -bf 0 实际无 B 帧，profile_idc 不能权威判定。SPS bitstream_restriction
    //   缺席（VUI 可选字段，MediaMTX/GStreamer 不发）也保持乐观；运行期 codec 在 emit 端
    //   做 PTS 反序检测，连续 N 次反序触发 controller 降级回调（关 LowLatency 重启 codec）。
    //
    // Color 三级兜底（color_meta::resolve_color）：VUI 自洽 → 启发式 → 用户覆盖。
    // 高度未知时默认 1080（启发式落 BT.709，符合现网 IPC 主流）。
    void decide_codec_policy(const transport::SdpMedia* /*video*/) noexcept {
        decoded_no_b_frames = true;     // 乐观默认：开 LowLatencyMode
        media::VuiInputs vui;
        uint32_t height_hint = 1080;

        if (video_codec == MC_VIDEO_CODEC_H264) {
            if (depack_h264) {
                const auto& info = depack_h264->sps_info();
                if (info.parsed) {
                    if (info.bitstream_restriction && info.max_num_reorder_frames > 0) {
                        decoded_no_b_frames = false;
                        MCP_LOGF(pal::LogLevel::info,
                                 "Controller: H.264 SPS bitstream_restriction reorder=%u → has B-frames, disable LowLatency",
                                 info.max_num_reorder_frames);
                    }
                    if (info.colour_primaries >= 0) {
                        vui.colour_primaries = info.colour_primaries;
                        vui.matrix_coefficients = info.matrix_coefficients;
                        vui.transfer_characteristics = info.transfer_characteristics;
                        vui.video_full_range_flag = info.video_full_range_flag;
                    }
                    if (info.pic_height_in_map_units > 0) {
                        height_hint = info.pic_height_in_map_units * 16u;
                    }
                    MCP_LOGF(pal::LogLevel::info,
                             "Controller: H.264 SPS reorder=%u brfp=%d vui_pri=%d mat=%d trc=%d full=%d",
                             info.max_num_reorder_frames,
                             info.bitstream_restriction ? 1 : 0,
                             info.colour_primaries, info.matrix_coefficients,
                             info.transfer_characteristics,
                             info.video_full_range_flag ? 1 : 0);
                }
            }
        } else if (video_codec == MC_VIDEO_CODEC_H265) {
            if (depack_h265) {
                const auto& info = depack_h265->sps_info();
                if (info.parsed) {
                    if (info.max_num_reorder_pics > 0) {
                        decoded_no_b_frames = false;
                        MCP_LOGF(pal::LogLevel::info,
                                 "Controller: H.265 SPS reorder=%u → has B-frames, disable LowLatency",
                                 info.max_num_reorder_pics);
                    }
                    if (info.pic_height > 0) height_hint = info.pic_height;
                    MCP_LOGF(pal::LogLevel::info,
                             "Controller: H.265 SPS reorder=%u h=%u",
                             info.max_num_reorder_pics, info.pic_height);
                }
            }
        }

        // Color：三级兜底由 resolve_color 内部完成。
        media::ColorOverride no_override{};
        resolved_color = media::resolve_color(vui, height_hint, no_override);

        // env escape valve:MCP_FORCE_LOWLATENCY=1 强制 LowLatency on,绕过 ADD §5.6.4
        // B-Frame Policy。背景:sync software MFT(IoT LTSC 兜底档)在 prefer_low_latency=
        // false 时强启 driver-side reorder buffer +66~100ms,VLC libavcodec 同流无此代价
        // (caller-side reorder)。本 env 让用户对照 VLC 实测能否在自家流上接受花屏风险换
        // 100ms 延时收益。SPS 报 reorder>0 但实际 -bf 0 的现网编码器命中此 env 不花屏。
        char ll_env[8]; size_t ll_n = 0;
        if (getenv_s(&ll_n, ll_env, sizeof(ll_env), "MCP_FORCE_LOWLATENCY") == 0
            && ll_n > 0 && ll_env[0] == '1' && !decoded_no_b_frames) {
            decoded_no_b_frames = true;
            MCP_LOG_WARN("Controller: MCP_FORCE_LOWLATENCY=1 → forcing LowLatency on "
                         "despite SPS reorder>0 (ADD §5.6.4 warns of garble risk on "
                         "real B-frame streams; recover by unsetting env)");
        }

        MCP_LOGF(pal::LogLevel::info,
                 "Controller: codec policy: low_latency=%d (default optimistic; runtime PTS-inversion fallback) "
                 "color(pri=%d range=%d mat=%d) h_hint=%u",
                 decoded_no_b_frames ? 1 : 0,
                 static_cast<int>(resolved_color.primaries),
                 static_cast<int>(resolved_color.range),
                 static_cast<int>(resolved_color.matrix),
                 height_hint);
    }

    // 限频包装:RFC 4585 §6.3.1 immediate feedback mode 下保持 minimum interval,
    // 否则 SEQ_GAP 风暴(每帧丢一包)会触发每帧一个 PLI → server 反向被打爆。
    // reason 仅用于 trace 日志区分首帧 vs SEQ_GAP vs 其他来源。controller 当前 RTP
    // 处理单线程,这里 atomic load+store 不做 CAS — 即使未来扩展多线程同时通过限频
    // 多发一两次 PLI 也不致命(server 端 PLI 幂等,只会刷一次 IDR)。
    void try_send_video_pli(uint32_t media_ssrc, const char* reason) noexcept {
        const int64_t now_ns  = pal::Clock::qpc_now_ns();
        const int64_t prev_ns = last_video_pli_ns.load(std::memory_order_acquire);
        if (prev_ns != 0 && (now_ns - prev_ns) < kPliMinIntervalNs) {
            return;
        }
        last_video_pli_ns.store(now_ns, std::memory_order_release);
        send_video_pli(media_ssrc);
        if (reason) {
            MCP_LOGF(pal::LogLevel::trace,
                     "Controller: PLI fired reason=%s media_ssrc=0x%08X "
                     "(min_interval=%lld ms)",
                     reason, media_ssrc,
                     static_cast<long long>(kPliMinIntervalNs / 1'000'000));
        }
    }

    // 构造 RFC 4585 §3.1 兼容的复合包(RR + SDES + PLI)并下发到 transport。
    // 仅 RTSP-TCP interleaved + RTSP-UDP(SETUP 后 prime peer_rtcp_addr 路径)会真的把
    // RTCP 送回摄像头;WHEP transport 当前 send_rtcp 返回 MC_ERR_UNSUPPORTED,调用方静默丢弃。
    void send_video_pli(uint32_t media_ssrc) noexcept {
        if (!transport) return;
        std::array<uint8_t, 64> buf{};
        std::size_t off = 0;
        const auto rr_len = transport::RtcpWriter::write_receiver_report(
            local_sender_ssrc, {}, std::span<uint8_t>{buf.data() + off, buf.size() - off});
        if (rr_len == 0) return;
        off += rr_len;
        constexpr char kCname[] = "mc-player";
        const auto sdes_len = transport::RtcpWriter::write_sdes_cname(
            local_sender_ssrc,
            std::span<const char>{kCname, sizeof(kCname) - 1},
            std::span<uint8_t>{buf.data() + off, buf.size() - off});
        if (sdes_len == 0) return;
        off += sdes_len;
        const auto pli_len = transport::RtcpWriter::write_pli(
            local_sender_ssrc, media_ssrc,
            std::span<uint8_t>{buf.data() + off, buf.size() - off});
        if (pli_len == 0) return;
        off += pli_len;
        const mc_status_t s = transport->send_rtcp(
            transport::MediaKind::video,
            std::span<const uint8_t>{buf.data(), off});
        // 性能量度规范 §4.3 mc.fb.pli_sent_count: PLI 发送（含 send_rtcp 失败的尝试）。
        pal::metric::Registry::instance().counter("mc.fb.pli_sent_count").inc();
        MCP_LOGF(pal::LogLevel::info,
                 "Controller: sent video PLI media_ssrc=0x%08X status=%d",
                 media_ssrc, static_cast<int>(s));
    }

    // RFC 4585 §6.2.1 Generic NACK — pid 是首个丢失 seq,blp 是后续 16 个 seq 的位图。
    // 由 jitter_buffer_video 的 NackFn callback 触发(检测到 gap 立即请求重传)。
    // 限频已在 jitter buffer 内做(同 pid nack_min_interval=50ms)。
    void send_video_nack(uint16_t pid, uint16_t blp) noexcept {
        if (!transport) return;
        const uint32_t media_ssrc = video_remote_ssrc.load(std::memory_order_acquire);
        if (media_ssrc == 0) return;     // 还没看到 RTP,SSRC 未知

        std::array<uint8_t, 64> buf{};
        std::size_t off = 0;
        const auto rr_len = transport::RtcpWriter::write_receiver_report(
            local_sender_ssrc, {}, std::span<uint8_t>{buf.data() + off, buf.size() - off});
        if (rr_len == 0) return;
        off += rr_len;
        constexpr char kCname[] = "mc-player";
        const auto sdes_len = transport::RtcpWriter::write_sdes_cname(
            local_sender_ssrc,
            std::span<const char>{kCname, sizeof(kCname) - 1},
            std::span<uint8_t>{buf.data() + off, buf.size() - off});
        if (sdes_len == 0) return;
        off += sdes_len;
        transport::RtcpNackEntry e{pid, blp};
        const auto nack_len = transport::RtcpWriter::write_nack(
            local_sender_ssrc, media_ssrc,
            std::span<const transport::RtcpNackEntry>{&e, 1},
            std::span<uint8_t>{buf.data() + off, buf.size() - off});
        if (nack_len == 0) return;
        off += nack_len;
        (void)transport->send_rtcp(transport::MediaKind::video,
                                   std::span<const uint8_t>{buf.data(), off});
        pal::metric::Registry::instance().counter("mc.fb.nack_sent_count").inc();
    }

    // 音频管线启动（v1：仅 AAC；G.711 LUT 后续接入）。
    mc_status_t start_audio_pipeline(const transport::SdpMedia& audio) noexcept {
        if (audio.rtpmap.empty()) return MC_OK;
        const auto& rt = audio.rtpmap.front();
        audio_clock_rate = rt.clock_rate_hz;
        audio_channels   = rt.channels ? rt.channels : 1;

        if (audio_codec != MC_AUDIO_CODEC_AAC &&
            audio_codec != MC_AUDIO_CODEC_G711_ALAW &&
            audio_codec != MC_AUDIO_CODEC_G711_ULAW) {
            MCP_LOGF(pal::LogLevel::warn,
                     "Controller: audio codec '%s' not implemented in v1; skip audio render",
                     rt.codec_name.c_str());
            return MC_OK;
        }

        // 渲染端先起，不论 codec 路径，避免后面 setup 失败时漏 cleanup。
        audio_render = std::make_unique<media::AudioRenderWasapi>();
        if (mc_status_t s = audio_render->start(audio_clock_rate, audio_channels); s != MC_OK) {
            MCP_LOGF(pal::LogLevel::warn,
                     "Controller: audio render start failed status=%d (silent playback)",
                     static_cast<int>(s));
            audio_render.reset();
            return MC_OK;     // 视频继续，无声不阻塞
        }

        if (audio_codec == MC_AUDIO_CODEC_G711_ALAW || audio_codec == MC_AUDIO_CODEC_G711_ULAW) {
            // G.711 走 LUT，无 depack / MFT；on_rtp 直接喂解码。
            MCP_LOGF(pal::LogLevel::info,
                     "Controller: audio pipeline ready (G.711 %s %u Hz / %u ch LUT)",
                     audio_codec == MC_AUDIO_CODEC_G711_ALAW ? "A-law" : "u-law",
                     audio_clock_rate, audio_channels);
            return MC_OK;
        }

        // —— AAC 路径 —————————————————————————————————————
        // fmtp: config=hex / sizelength / indexlength。
        std::string asc_hex;
        uint32_t size_len = 13, index_len = 3;
        for (const auto& fmtp : audio.fmtp) {
            if (auto it = fmtp.params.find("config"); it != fmtp.params.end()) asc_hex = it->second;
            if (auto it = fmtp.params.find("sizelength"); it != fmtp.params.end())
                size_len = static_cast<uint32_t>(std::strtoul(it->second.c_str(), nullptr, 10));
            if (auto it = fmtp.params.find("indexlength"); it != fmtp.params.end())
                index_len = static_cast<uint32_t>(std::strtoul(it->second.c_str(), nullptr, 10));
        }

        media::CodecMftAudio::Config acfg;
        acfg.codec       = MC_AUDIO_CODEC_AAC;
        acfg.sample_rate = audio_clock_rate;
        acfg.channels    = audio_channels;
        acfg.emit        = [this](media::AudioFrame&& f) {
            if (audio_render) audio_render->submit(std::move(f));
        };
        codec_audio = std::make_unique<media::CodecMftAudio>(std::move(acfg));

        // 解析 asc hex 到 bytes 临时缓冲（DepackAac 也持有，但 MFT 需要原始 bytes）。
        std::vector<uint8_t> asc;
        asc.reserve(asc_hex.size() / 2);
        auto hex_v = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = -1;
        for (char c : asc_hex) {
            int v = hex_v(c); if (v < 0) continue;
            if (hi < 0) hi = v;
            else { asc.push_back(static_cast<uint8_t>((hi << 4) | v)); hi = -1; }
        }

        if (mc_status_t s = codec_audio->start(asc); s != MC_OK) {
            MCP_LOGF(pal::LogLevel::warn,
                     "Controller: AAC MFT start failed status=%d", static_cast<int>(s));
            codec_audio.reset();
            audio_render.reset();
            return MC_OK;
        }

        depack_aac = std::make_unique<media::DepackAac>(
            [this](media::AacAccessUnit&& au) {
                static std::atomic<uint64_t> n{0};
                if (n.fetch_add(1, std::memory_order_relaxed) == 0) {
                    MCP_LOGF(pal::LogLevel::info,
                             "Controller: first AAC AU from depack (size=%zu pts=%lld)",
                             au.raw_aac.size(), static_cast<long long>(au.pts_us));
                }
                if (codec_audio) codec_audio->submit(au.raw_aac, au.pts_us);
            });
        depack_aac->set_audio_specific_config_hex(asc_hex);
        depack_aac->set_au_header_lengths(size_len, index_len);

        MCP_LOGF(pal::LogLevel::info,
                 "Controller: audio pipeline ready (AAC sdp_rate=%u ch=%u, asc=%zu bytes, "
                 "sizelen=%u indexlen=%u)",
                 audio_clock_rate, audio_channels, asc.size(), size_len, index_len);
        return MC_OK;
    }

    mc_status_t start_decode_pipeline() noexcept {
        if (!d3d_device) return MC_ERR_INVALID_STATE;
        if (!render_hwnd) {
            MCP_LOGF(pal::LogLevel::warn,
                     "Controller: render_hwnd is null, frames will be dropped");
        }

        // 渲染先起，作为 admit 的 sink。
        media::RenderD3d11::Config rcfg;
        rcfg.device       = d3d_device;
        rcfg.hwnd         = render_hwnd;
        rcfg.profile_hint = options.render_profile_hint;
        render = std::make_unique<media::RenderD3d11>(std::move(rcfg));
        if (mc_status_t s = render->start(); s != MC_OK) {
            MCP_LOGF(pal::LogLevel::error,
                     "Controller: render start failed status=%d", static_cast<int>(s));
            return s;
        }

        // render 启动后立即把当前 state / URL 同步到 UI overlay（emit_state_change 在
        // render 创建前已发生第一次 CONNECTING，那次 set_stage 是 noop）。
        if (auto* ui = render->ui_overlay()) {
            ui->set_stage(map_state_to_ui_stage(state.load(std::memory_order_acquire)));
            ui->set_url(utf8_to_wide(options.url ? options.url : ""));
        }

        gate = std::make_unique<media::FrameValidityGate>(
            [this](const media::VideoFrame& f) { on_admitted(f); });

        // ADR-015 四级降级链 (plan Phase 1)：vendor SDK → DXVA-direct → MFT hardware async → libcodec
        // 原则：硬件 path 最短优先（host 抽象层数从短到长）。前一档失败立即降级，每档独立 probe。
        // tier_skip_reason / tierN_ns / tier_selected metric 上报对位性能量度规范 §2.4。
        auto& reg = pal::metric::Registry::instance();

        auto record_skip = [&reg](int tier, const char* reason) {
            char name[96];
            std::snprintf(name, sizeof(name),
                          "mc.probe.tier_skip_reason.tier%d.%s", tier, reason);
            reg.counter(name).inc();
            char count_name[64];
            std::snprintf(count_name, sizeof(count_name),
                          "mc.probe.tier%d_skip_count", tier);
            reg.counter(count_name).inc();
        };

        auto activate = [&reg, this](mc_decoder_kind_t k, int tier) {
            decoder_kind = k;
            reg.gauge("mc.decoder.kind").set(k);
            reg.gauge("mc.probe.tier_selected").set(tier);
        };

        // 档 1: Vendor SDK 直驱(NVDEC / oneVPL / AMF) — ADR-015 §5.6.2.1。
        // 多档 vendor 互斥(VendorId 路由),按本机 adapter 选择适配档:
        //   - NVIDIA 0x10DE → NVDEC (Phase 5a 骨架,5b 待补 cuvid 解码循环)
        //   - Intel  0x8086 → oneVPL (Phase 6a 骨架,6b 待补 MFX 解码循环)
        //   - AMD    0x1002 → AMF   (Phase 7 留)
        //   - 其它           → vendor_mismatch
        //
        // 每档 codec impl 自己探测 VendorId 不命中即返 MC_ERR_UNSUPPORTED + 设
        // last_start_reason=vendor_mismatch;controller 按 reason_label 填 metric。
        // 失败兜底降到 tier 2 DXVA-direct(Phase 4 已 H.264/H.265 全覆盖)。
        {
            pal::metric::ScopedTimer t{reg.timer("mc.probe.tier1_ns")};
            // 子档轮询 vendor_mismatch 不计数(它是路由不命中,不是 SDK 故障)。
            // 仅适配档(VendorId 命中)的失败 reason 才入 skip metric。
            // 全档都 vendor_mismatch 时 fallback 记一条 vendor_mismatch。
            bool tier1_recorded = false;

            // NVDEC(NV 0x10DE 命中)
            {
                media::CodecNvdec::Config ncfg;
                ncfg.codec  = video_codec;
                ncfg.device = d3d_device;
                ncfg.emit   = [this](media::VideoFrame&& f) { on_decoded_frame(std::move(f)); };
                codec_nvdec = std::make_unique<media::CodecNvdec>(std::move(ncfg));
                const mc_status_t s = codec_nvdec->start();
                if (s == MC_OK) {
                    MCP_LOGF(pal::LogLevel::info, "Controller: tier1 NVDEC active");
                    activate(MC_DECODER_VENDOR_SDK_NVDEC, 1);
                    return MC_OK;
                }
                const auto reason = codec_nvdec->last_start_reason();
                if (reason != media::CodecNvdec::StartReason::vendor_mismatch) {
                    record_skip(1, media::CodecNvdec::reason_label(reason));
                    tier1_recorded = true;
                    MCP_LOGF(pal::LogLevel::info,
                             "Controller: tier1 NVDEC skip reason=%s (status=%d)",
                             media::CodecNvdec::reason_label(reason), static_cast<int>(s));
                }
                codec_nvdec.reset();
            }

            // oneVPL(Intel 0x8086 命中)
            if (!tier1_recorded) {
                media::CodecOneVPL::Config ocfg;
                ocfg.codec  = video_codec;
                ocfg.device = d3d_device;
                ocfg.emit   = [this](media::VideoFrame&& f) { on_decoded_frame(std::move(f)); };
                codec_onevpl = std::make_unique<media::CodecOneVPL>(std::move(ocfg));
                const mc_status_t s = codec_onevpl->start();
                if (s == MC_OK) {
                    MCP_LOGF(pal::LogLevel::info, "Controller: tier1 oneVPL active");
                    activate(MC_DECODER_VENDOR_SDK_ONEVPL, 1);
                    return MC_OK;
                }
                const auto reason = codec_onevpl->last_start_reason();
                if (reason != media::CodecOneVPL::StartReason::vendor_mismatch) {
                    record_skip(1, media::CodecOneVPL::reason_label(reason));
                    tier1_recorded = true;
                    MCP_LOGF(pal::LogLevel::info,
                             "Controller: tier1 oneVPL skip reason=%s (status=%d)",
                             media::CodecOneVPL::reason_label(reason), static_cast<int>(s));
                }
                codec_onevpl.reset();
            }

            // AMF(AMD 0x1002 命中)
            if (!tier1_recorded) {
                media::CodecAmf::Config acfg;
                acfg.codec  = video_codec;
                acfg.device = d3d_device;
                acfg.emit   = [this](media::VideoFrame&& f) { on_decoded_frame(std::move(f)); };
                codec_amf   = std::make_unique<media::CodecAmf>(std::move(acfg));
                const mc_status_t s = codec_amf->start();
                if (s == MC_OK) {
                    MCP_LOGF(pal::LogLevel::info, "Controller: tier1 AMF active");
                    activate(MC_DECODER_VENDOR_SDK_AMF, 1);
                    return MC_OK;
                }
                const auto reason = codec_amf->last_start_reason();
                if (reason != media::CodecAmf::StartReason::vendor_mismatch) {
                    record_skip(1, media::CodecAmf::reason_label(reason));
                    tier1_recorded = true;
                    MCP_LOGF(pal::LogLevel::info,
                             "Controller: tier1 AMF skip reason=%s (status=%d)",
                             media::CodecAmf::reason_label(reason), static_cast<int>(s));
                }
                codec_amf.reset();
            }

            // 全档非匹配兜底(罕见 adapter,如 ARM Mali / 软件 WARP)
            if (!tier1_recorded) {
                record_skip(1, "vendor_mismatch");
            }
        }

        // 档 2: DXVA-direct（D3D11VideoDevice）— HEVC 已实装；H.264 留 Phase 4 stub。
        // MCP_SKIP_TIER2 调试开关:跳过 DXVA-direct,直接走 tier 3 MFT 对照测试 silent fail。
        const bool skip_tier2 = []{
            char buf[8]; size_t n = 0;
            return getenv_s(&n, buf, sizeof(buf), "MCP_SKIP_TIER2") == 0 && n > 0 && buf[0] == '1';
        }();
        if (!skip_tier2) {
            pal::metric::ScopedTimer t{reg.timer("mc.probe.tier2_ns")};
            if (video_codec == MC_VIDEO_CODEC_H265) {
                media::CodecDxvaVideo::Config dcfg;
                dcfg.codec  = video_codec;
                dcfg.device = d3d_device;
                dcfg.emit   = [this](media::VideoFrame&& f) { on_decoded_frame(std::move(f)); };
                codec_dxva  = std::make_unique<media::CodecDxvaVideo>(std::move(dcfg));
                if (mc_status_t s = codec_dxva->start(); s == MC_OK) {
                    MCP_LOGF(pal::LogLevel::info,
                             "Controller: tier2 DXVA-direct H.265 active "
                             "(HEVC Extension not required)");
                    activate(MC_DECODER_DXVA_DIRECT, 2);
                    return MC_OK;
                } else {
                    MCP_LOGF(pal::LogLevel::warn,
                             "Controller: tier2 DXVA-direct H.265 start failed status=%d",
                             static_cast<int>(s));
                    codec_dxva.reset();
                    record_skip(2, "profile_unsupported");
                }
            } else if (video_codec == MC_VIDEO_CODEC_H264) {
                // ADR-015 capability-then-select(用户要求"完备硬件能力检查后 再决定是否回退"):
                //   driver CheckVideoDecoderFormat 返 TRUE 不等于真能解码 — Intel UHD 730 +
                //   Win11 IoT LTSC 上观察到 self-test fixture(64x64 baseline IDR)在 driver
                //   上 fail,但实流 1280x720 high profile 直接 d3d11va 完美解码(VLC 实证)。
                //
                //   即:fixture self-test 是 false negative,过严判据反而把能用的硬件挡掉,
                //   逼用户跌到 tier 3 sync software MFT(driver-side B-frame reorder 累
                //   ~664ms latency)。
                //
                // 修订策略(2026-05-07 实证后,见 ADR-022):
                //   早期版本(74acb24)默认信任 driver,基于 VLC 同硬件命中 d3d11va 推测
                //   driver 可用。后续截图验证发现 mc-player tier 2 路径在 Intel UHD 730
                //   + Win11 IoT LTSC 上 driver silent fail(SubmitDecoderBuffers 返 OK
                //   但 NV12 全 0 → 深绿屏),即便已对照 ffmpeg/VLC 修 GUID priority +
                //   IQMatrix + DPB=24 + Reserved16Bits=3 + video session mutex 也未解。
                //   差异定位在 PicParams 某未知字段。
                //
                //   现行默认:严格 probe gate — fixture probe fail 即降 tier 3(画面正常,
                //   有 +960ms latency 但胜过深绿)。VLC 行为对齐留 ADR-022 跟进。
                //
                // env 控制:
                //   MCP_ENABLE_DXVA_H264=1: 强制启用 tier 2(高级用户实测 driver 可用时用)
                //   MCP_ENABLE_DXVA_H264=0: 强制禁用 tier 2(同当前默认 + 显式)
                //   MCP_TRUST_DRIVER_DXVA=1: 历史"信任 driver"行为(probe 仅诊断,不阻断)
                //   未设置: 默认严格(probe fail = 降 tier 3 画面正常)
                char env_buf[8]; size_t env_n = 0;
                const bool env_set = getenv_s(&env_n, env_buf, sizeof(env_buf),
                                               "MCP_ENABLE_DXVA_H264") == 0 && env_n > 0;
                const bool env_force_on  = env_set && env_buf[0] == '1';
                const bool env_force_off = env_set && env_buf[0] == '0';

                char td_buf[8]; size_t td_n = 0;
                const bool trust_driver = (getenv_s(&td_n, td_buf, sizeof(td_buf),
                                                    "MCP_TRUST_DRIVER_DXVA") == 0
                                           && td_n > 0 && td_buf[0] == '1');

                bool capable;
                const char* skip_reason = nullptr;
                if (env_force_off) {
                    capable     = false;
                    skip_reason = "disabled_by_env";
                    MCP_LOG_INFO("Controller: tier2 DXVA H.264 disabled "
                                 "(MCP_ENABLE_DXVA_H264=0 force disable)");
                } else if (env_force_on) {
                    capable = true;
                    MCP_LOG_INFO("Controller: tier2 DXVA H.264 forced on "
                                 "(MCP_ENABLE_DXVA_H264=1)");
                } else {
                    pal::metric::ScopedTimer pt{reg.timer("mc.probe.tier2_h264_self_test_ns")};
                    const bool probe_pass = media::probe_dxva_h264_capable(d3d_device.Get());
                    if (trust_driver) {
                        capable = true;
                        MCP_LOGF(pal::LogLevel::info,
                                 "Controller: tier2 DXVA H.264 fixture probe=%d "
                                 "(MCP_TRUST_DRIVER_DXVA=1: bypass probe, may green-screen)",
                                 probe_pass);
                    } else {
                        capable = probe_pass;
                        if (!capable) skip_reason = "self_test_fail_silent_decode";
                        MCP_LOGF(pal::LogLevel::info,
                                 "Controller: tier2 DXVA H.264 strict probe (default since ADR-022) "
                                 "probe=%d → capable=%d",
                                 probe_pass, capable);
                    }
                }

                if (capable) {
                    media::CodecDxvaVideo::Config dcfg;
                    dcfg.codec  = video_codec;
                    dcfg.device = d3d_device;
                    dcfg.emit   = [this](media::VideoFrame&& f) { on_decoded_frame(std::move(f)); };
                    codec_dxva  = std::make_unique<media::CodecDxvaVideo>(std::move(dcfg));
                    if (mc_status_t s = codec_dxva->start(); s == MC_OK) {
                        MCP_LOG_INFO("Controller: tier2 DXVA-direct H.264 active");
                        activate(MC_DECODER_DXVA_DIRECT, 2);
                        return MC_OK;
                    } else {
                        MCP_LOGF(pal::LogLevel::warn,
                                 "Controller: tier2 DXVA-direct H.264 start failed status=%d",
                                 static_cast<int>(s));
                        codec_dxva.reset();
                        record_skip(2, "start_failed");
                    }
                } else {
                    record_skip(2, skip_reason ? skip_reason : "self_test_fail_silent_decode");
                }
            } else {
                record_skip(2, "codec_unsupported");
            }
        } else {
            record_skip(2, "skipped_by_env");
            MCP_LOG_INFO("Controller: tier2 DXVA-direct skipped via MCP_SKIP_TIER2=1");
        }

        // 档 3: MFT — 优先 hardware async (ADR-015 SDI 标准延时);若不存在,接受 sync software
        // (Microsoft H.264/H.265 Decoder MFT)作为 IoT LTSC 等"组件不齐"环境的功能兜底。
        // sync software 在 reorder>0 流上 +100ms 延时,但仍优于黑屏 + 优于 stub libcodec。
        {
            pal::metric::ScopedTimer t{reg.timer("mc.probe.tier3_ns")};
            const auto reason = probe_mft_video_async_hardware(video_codec);
            if (reason == MftProbeReason::hardware_async_available ||
                reason == MftProbeReason::sync_software_available) {
                media::CodecMftVideo::Config ccfg;
                ccfg.codec  = video_codec;
                ccfg.device = d3d_device;
                ccfg.prefer_low_latency = decoded_no_b_frames;     // ADD §5.6.4 B-Frame Policy
                ccfg.color  = resolved_color;                       // ADD §5.12 三级兜底结果
                ccfg.emit   = [this](media::VideoFrame&& f) { on_decoded_frame(std::move(f)); };
                codec_video = std::make_unique<media::CodecMftVideo>(std::move(ccfg));
                if (mc_status_t s = codec_video->start({}); s == MC_OK) {
                    const bool is_async = (reason == MftProbeReason::hardware_async_available);
                    MCP_LOGF(pal::LogLevel::info,
                             "Controller: tier3 MFT %s active",
                             is_async ? "hardware async" : "sync software (IoT LTSC fallback)");
                    activate(is_async ? MC_DECODER_MFT_HARDWARE : MC_DECODER_MFT_SOFTWARE, 3);
                    return MC_OK;
                } else {
                    MCP_LOGF(pal::LogLevel::warn,
                             "Controller: tier3 MFT codec start failed status=%d",
                             static_cast<int>(s));
                    codec_video.reset();
                    record_skip(3, "activate_failed");
                }
            } else {
                record_skip(3, mft_probe_reason_str(reason));
                MCP_LOGF(pal::LogLevel::info,
                         "Controller: tier3 MFT skipped reason=%s",
                         mft_probe_reason_str(reason));
            }
        }

        // 档 4: mc-libcodec 软解（final fallback）。allow_software_decode=0 时上报 NO_HARDWARE。
        if (!options.allow_software_decode) {
            record_skip(4, "disabled_by_caller");
            return MC_ERR_NO_HARDWARE;
        }

        media::CodecLibcodecVideo::Config lcfg;
        lcfg.codec  = video_codec;
        lcfg.device = d3d_device;
        lcfg.emit   = [this](media::VideoFrame&& f) { on_decoded_frame(std::move(f)); };
        codec_libcodec = std::make_unique<media::CodecLibcodecVideo>(std::move(lcfg));
        if (mc_status_t s = codec_libcodec->start(); s != MC_OK) {
            MCP_LOGF(pal::LogLevel::error,
                     "Controller: tier4 libcodec start failed status=%d",
                     static_cast<int>(s));
            codec_libcodec.reset();
            decoder_kind = MC_DECODER_NONE;
            reg.gauge("mc.decoder.kind").set(decoder_kind);
            record_skip(4, "start_failed");
            return s;
        }
        MCP_LOGF(pal::LogLevel::info,
                 "Controller: tier4 mc-libcodec software decoder active");
        activate(MC_DECODER_LIBCODEC, 4);
        return MC_OK;
    }

    void on_stream_ready(const transport::StreamDescription& desc) noexcept {
        const transport::SdpMedia* video = find_media(desc.sdp, transport::SdpMedia::Kind::video);
        if (!video || video->rtpmap.empty()) {
            emit_error(MC_ERR_PROTOCOL, "no video media in SDP");
            return;
        }
        video_codec = map_video_codec(video->rtpmap.front().codec_name);
        if (video_codec == MC_VIDEO_CODEC_UNKNOWN) {
            emit_error(MC_ERR_UNSUPPORTED,
                        std::string{"unsupported video codec: "} + video->rtpmap.front().codec_name);
            return;
        }

        // 选 adapter → 建 D3D11 device → 启 decode/render。
        if (auto p = AdapterPicker::pick(caps_probe, render_hwnd, video_codec, 0, 0); p) {
            adapter_pick = *p;
        }
        if (mc_status_t s = create_d3d_device(); s != MC_OK) {
            emit_error(s, "D3D11 device creation failed");
            return;
        }

        if (video_codec == MC_VIDEO_CODEC_H264) {
            depack_h264 = std::make_unique<media::DepackH264>(
                [this](media::H264AccessUnit&& au) { on_video_au_h264(std::move(au)); });
            for (const auto& fmtp : video->fmtp) {
                if (auto it = fmtp.params.find("sprop-parameter-sets"); it != fmtp.params.end()) {
                    depack_h264->set_sprop_parameter_sets(it->second);
                    break;
                }
            }
            open_dump_if_enabled("mc-player-dump.h264");
        } else if (video_codec == MC_VIDEO_CODEC_H265) {
            depack_h265 = std::make_unique<media::DepackH265>(
                [this](media::H265AccessUnit&& au) { on_video_au_h265(std::move(au)); });
            for (const auto& fmtp : video->fmtp) {
                if (auto it = fmtp.params.find("sprop-vps"); it != fmtp.params.end())
                    depack_h265->set_sprop_vps(it->second);
                if (auto it = fmtp.params.find("sprop-sps"); it != fmtp.params.end())
                    depack_h265->set_sprop_sps(it->second);
                if (auto it = fmtp.params.find("sprop-pps"); it != fmtp.params.end())
                    depack_h265->set_sprop_pps(it->second);
            }
            open_dump_if_enabled("mc-player-dump.h265");
        } else {
            emit_error(MC_ERR_UNSUPPORTED, "v1 仅实装 H.264/H.265");
            return;
        }

        // depack 已注入 sprop（H.264 sps/pps 或 H.265 vps/sps/pps），SPS 已解析就绪。
        // 综合 SDP profile-level-id + SPS VUI 决定 LowLatencyMode 是否启用 + 色彩三级兜底。
        decide_codec_policy(video);

        // RTP 视频 jitter buffer:reorder window + RTCP NACK + 真丢包 loss 上报。
        // 三个 callback:
        //   - emit:按 seq 升序、连续输出 → 解 RTP header → 喂 depack
        //   - nack:发现 gap → 立即 NACK pid+blp(限频在 jitter buffer 内做)
        //   - loss:dwell 超时仍未到的 seq → poison gate + PLI(对齐旧 SEQ_GAP 路径)
        // 单 seq 乱序 / 可补回的丢包在 jitter buffer 内吸收,不再触发 gate freeze;
        // 真丢失才 freeze + 等下个 IDR(对齐 ADR-014 strict 但减少触发频次)。
        jitter_video = std::make_unique<media::JitterBufferVideo>(
            [this](const transport::RtpDatagram& d) { on_video_rtp_jittered(d); },
            [this](uint16_t pid, uint16_t blp) { send_video_nack(pid, blp); },
            [this](uint16_t seq, uint32_t cnt)  { on_video_rtp_loss(seq, cnt); });
        // dwell 30ms = 95% LAN-IPC reorder 容忍 + 留 ~RTT 让 NACK 重传到达。
        jitter_video->configure(30'000, 50'000);

        if (mc_status_t s = start_decode_pipeline(); s != MC_OK) {
            std::string msg = "decode pipeline start failed";
            if (s == MC_ERR_NO_HARDWARE && video_codec == MC_VIDEO_CODEC_H265) {
                msg += " (HEVC: 系统未装 HEVC Extension 且 allow_software_decode=0；"
                        "开启软件解码或装 Microsoft HEVC 扩展。参见 ADR-004)";
            }
            emit_error(s, std::move(msg));
            return;
        }

        const mc_state_t prev = state.exchange(MC_STATE_PLAYING, std::memory_order_acq_rel);
        emit_state_change(prev, MC_STATE_PLAYING);

        Event e;
        e.type                = MC_EVENT_STREAM_INFO;
        e.new_state           = MC_STATE_PLAYING;
        e.prev_state          = MC_STATE_PLAYING;
        e.stream_info_valid   = true;
        e.stream_info.struct_size    = sizeof(mc_stream_info_t);
        e.stream_info.struct_version = MC_STREAM_INFO_VERSION;
        e.stream_info.video_codec        = video_codec;
        e.stream_info.video_decoder_kind = decoder_kind;
        // 协议跟随 caller hint:AUTO / RTSP_UDP 走 UDP transport,RTSP_TCP 走 TCP interleaved。
        // make_rtsp_transport 当前实装与此对应(详见 ts_rtsp_udp.cpp::make_rtsp_transport)。
        // TODO: UDP→TCP 自动 fallback 落地后需在 transport 上报实际选择,这里同步更新。
        e.stream_info.active_protocol    =
            (options.protocol_hint == MC_PROTOCOL_RTSP_TCP)
                ? MC_PROTOCOL_RTSP_TCP : MC_PROTOCOL_RTSP_UDP;

        // GPU 信息：从 caps_probe 取所选 adapter 的描述与 VRAM 分类。
        e.stream_info.gpu_kind = MC_GPU_KIND_UNKNOWN;
        e.stream_info.gpu_description[0] = '\0';
        if (const auto* picked_caps = caps_probe.find_by_luid(adapter_pick.luid)) {
            e.stream_info.gpu_kind = classify_gpu(*picked_caps);
            copy_adapter_description(e.stream_info.gpu_description, picked_caps->description);
        }
        if (auto* audio = find_media(desc.sdp, transport::SdpMedia::Kind::audio); audio && !audio->rtpmap.empty()) {
            const auto& a = audio->rtpmap.front();
            e.stream_info.audio_sample_rate_hz = a.clock_rate_hz;
            e.stream_info.audio_channels       = a.channels;
            if (a.codec_name == "MPEG4-GENERIC")        e.stream_info.audio_codec = MC_AUDIO_CODEC_AAC;
            else if (a.codec_name == "PCMA")            e.stream_info.audio_codec = MC_AUDIO_CODEC_G711_ALAW;
            else if (a.codec_name == "PCMU")            e.stream_info.audio_codec = MC_AUDIO_CODEC_G711_ULAW;
            audio_codec = e.stream_info.audio_codec;
            // 异步启动音频管线：把 ~100-200 ms 的 WASAPI Initialize 移出 transport 线程，
            // depack_h264/265 + codec_video 在本函数返回后立即可服务视频 RTP；audio_render /
            // depack_aac / codec_audio 由子线程稍后填充，on_rtp 中已有 nullptr 兜底。
            // 写入 unique_ptr 成员在 x64 上是 word-aligned atomic store，与 on_rtp 的读取不会撕裂；
            // 唯一需要确保的是 close() 在持 mu 前先 join 该线程，避免双写竞争。
            transport::SdpMedia audio_copy = *audio;
            if (audio_start_thread.joinable()) audio_start_thread.join();
            audio_start_thread = std::thread([this, am = std::move(audio_copy)]() {
                (void)start_audio_pipeline(am);
            });
        }
        // 把 stream_info 也推到 UI overlay：playing stage 的 codec / 分辨率 / fps / GPU 标签来源。
        if (render && render->ui_overlay()) {
            render->ui_overlay()->set_stream_info(e.stream_info);
        }
        if (sink) sink(e);
    }

    // jitter_buffer_video::EmitFn — 按 seq 升序、连续输出后才解 RTP 内容并喂 depack。
    // 单 seq 乱序 / NACK 重传到达的包在 jitter buffer 内被 reorder/去重,这里收到的
    // 一定是 sequence 连续的(中间真丢失的 seq 已由 loss callback 上报 + 跳过)。
    void on_video_rtp_jittered(const transport::RtpDatagram& dg) noexcept {
        if (dg.bytes.size() < 12) return;
        const uint8_t* p  = dg.bytes.data();
        const bool marker = (p[1] & 0x80) != 0;
        const uint32_t ts = (static_cast<uint32_t>(p[4]) << 24) |
                             (static_cast<uint32_t>(p[5]) << 16) |
                             (static_cast<uint32_t>(p[6]) <<  8) |
                              static_cast<uint32_t>(p[7]);
        const uint8_t cc = p[0] & 0x0F;
        std::size_t off = 12 + cc * 4;
        if ((p[0] & 0x10) != 0 && off + 4 <= dg.bytes.size()) {
            const uint16_t ext_len = static_cast<uint16_t>(
                (static_cast<uint16_t>(p[off + 2]) << 8) | p[off + 3]);
            off += 4 + static_cast<std::size_t>(ext_len) * 4u;
        }
        if (off >= dg.bytes.size()) return;
        std::span<const uint8_t> payload(p + off, dg.bytes.size() - off);

        // 视频时钟固定 90 kHz:us = ts * 1e6 / 90000 = ts * 100 / 9
        const int64_t pts_us = static_cast<int64_t>(ts) * 100 / 9;

        if (depack_h264)      depack_h264->on_rtp(pts_us, marker, payload, dg.arrival_qpc_ns);
        else if (depack_h265) depack_h265->on_rtp(pts_us, marker, payload, dg.arrival_qpc_ns);
    }

    // jitter_buffer_video::LossFn — 真丢包(超过 reorder window 仍未到)。
    // 此时参考链可能断;按 ADR-014 strict gate poison + depack mark_reference_lost +
    // 紧急 PLI 让 server 下 IDR。注意单 seq 乱序、可 NACK 补回的丢包在 jitter buffer
    // 内已被吸收,不会触发本回调 — 即不再因抖动/小丢包冻结画面。
    void on_video_rtp_loss(uint16_t expired_seq, uint32_t count) noexcept {
        pal::metric::Registry::instance()
            .counter("mc.tput.rtp_loss_count").inc(static_cast<uint64_t>(count));

        char env_buf[8]; size_t env_n = 0;
        const bool seqgap_poison_disabled =
            getenv_s(&env_n, env_buf, sizeof(env_buf), "MCP_DISABLE_SEQGAP_POISON") == 0
            && env_n > 0 && env_buf[0] == '1';
        if (!seqgap_poison_disabled) {
            if (gate) gate->mark_poisoned(MC_GATE_POISON_SEQ_GAP);
            if (depack_h264) depack_h264->mark_reference_lost();
            if (depack_h265) depack_h265->mark_reference_lost();
            const uint32_t ssrc = video_remote_ssrc.load(std::memory_order_acquire);
            if (ssrc != 0) {
                try_send_video_pli(ssrc, "jitter_loss");
            }
        }
        MCP_LOGF(pal::LogLevel::warn,
                 "Controller: jitter buffer loss seq=%u count=%u%s",
                 expired_seq, count,
                 seqgap_poison_disabled ? " [poison disabled by env]" : "");
    }

    void on_rtp(const transport::RtpDatagram& dg) noexcept {
        rtp_packets.fetch_add(1, std::memory_order_relaxed);
        if (dg.bytes.size() < 12) return;

        const uint8_t* p = dg.bytes.data();

        if (dg.kind == transport::MediaKind::video) {
            // 首个视频 RTP:学到 remote SSRC 并触发首次 PLI。
            // SSRC 学习不能延迟到 jitter buffer emit,因为乱序情况下首包可能 hold 在
            // buffer 里;现在直接从 raw datagram 学(jitter buffer 不影响 SSRC 字段)。
            const uint32_t media_ssrc = (static_cast<uint32_t>(p[8])  << 24) |
                                          (static_cast<uint32_t>(p[9])  << 16) |
                                          (static_cast<uint32_t>(p[10]) <<  8) |
                                           static_cast<uint32_t>(p[11]);
            video_remote_ssrc.store(media_ssrc, std::memory_order_release);
            if (!first_pli_sent.test_and_set(std::memory_order_acq_rel)) {
                try_send_video_pli(media_ssrc, "first_rtp");
            }

            // 喂 jitter buffer:它会按 seq reorder + 触发 NACK + 真丢包时调 loss callback。
            // jitter_video 不存在(早期 bootstrap)的 fallback 直接喂 depack。
            if (jitter_video) {
                jitter_video->on_rtp(dg);
            } else {
                on_video_rtp_jittered(dg);
            }
        } else if (dg.kind == transport::MediaKind::audio) {
            // 音频路径不走 jitter buffer(AAC frame 通常小、丢包对音频影响 ≪ 视频参考链);
            // 直接解 RTP header 喂 depack/G.711 LUT。
            const bool marker = (p[1] & 0x80) != 0;
            const uint32_t ts = (static_cast<uint32_t>(p[4]) << 24) |
                                 (static_cast<uint32_t>(p[5]) << 16) |
                                 (static_cast<uint32_t>(p[6]) <<  8) |
                                  static_cast<uint32_t>(p[7]);
            const uint8_t cc = p[0] & 0x0F;
            std::size_t off = 12 + cc * 4;
            if ((p[0] & 0x10) != 0 && off + 4 <= dg.bytes.size()) {
                const uint16_t ext_len = static_cast<uint16_t>(
                    (static_cast<uint16_t>(p[off + 2]) << 8) | p[off + 3]);
                off += 4 + static_cast<std::size_t>(ext_len) * 4u;
            }
            if (off >= dg.bytes.size()) return;
            std::span<const uint8_t> payload(p + off, dg.bytes.size() - off);

            static std::atomic<uint64_t> n{0};
            const auto cnt = n.fetch_add(1, std::memory_order_relaxed) + 1;
            if (cnt == 1 || cnt == 10 || cnt == 50 || (cnt % 200) == 0) {
                MCP_LOGF(pal::LogLevel::info,
                         "Controller: audio RTP #%llu (bytes=%zu marker=%d)",
                         static_cast<unsigned long long>(cnt),
                         dg.bytes.size(), marker ? 1 : 0);
            }
            if (audio_clock_rate == 0) return;
            const int64_t pts_us = static_cast<int64_t>(ts) * 1'000'000 / audio_clock_rate;

            if (audio_codec == MC_AUDIO_CODEC_G711_ALAW ||
                audio_codec == MC_AUDIO_CODEC_G711_ULAW) {
                if (!audio_render) return;
                media::AudioFrame af;
                af.pts_us         = pts_us;
                af.sample_rate_hz = audio_clock_rate;
                af.channels       = audio_channels;
                if (audio_codec == MC_AUDIO_CODEC_G711_ALAW) {
                    media::g711_alaw_decode(payload, af.pcm_interleaved);
                } else {
                    media::g711_ulaw_decode(payload, af.pcm_interleaved);
                }
                audio_render->submit(std::move(af));
            } else if (depack_aac) {
                depack_aac->on_rtp(pts_us, payload);
            }
        }
    }

    void on_error(mc_status_t code, std::string msg) noexcept {
        emit_error(code, std::move(msg));
    }

    mc_status_t open_unlocked(const mc_open_options_t& opt) noexcept {
        if (!opt.url || opt.url[0] == '\0') {
            return MC_ERR_INVALID_ARG;
        }
        options = opt;

        const mc_state_t prev = state.exchange(MC_STATE_CONNECTING, std::memory_order_acq_rel);
        emit_state_change(prev, MC_STATE_CONNECTING);

        // 每会话随机化本端 sender SSRC（RFC 3550 §8.1：避免与远端冲突）。
        {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<uint32_t> dis(1, 0xFFFFFFFFu);
            local_sender_ssrc = dis(gen);
        }
        first_pli_sent.clear(std::memory_order_release);
        video_remote_ssrc.store(0, std::memory_order_release);
        last_video_pli_ns.store(0, std::memory_order_release);

        if (mc_status_t s = caps_probe.probe(); s != MC_OK) {
            emit_error(s, "DxgiCapsProbe failed");
            return s;
        }

        transport = transport::make_rtsp_transport(opt.protocol_hint, opt.allow_tcp_fallback != 0);
        if (!transport) {
            emit_error(MC_ERR_INTERNAL, "make_rtsp_transport returned null");
            return MC_ERR_INTERNAL;
        }

        transport::TransportSessionSink sink_cb;
        sink_cb.on_stream_ready = [this](const transport::StreamDescription& d) { on_stream_ready(d); };
        sink_cb.on_rtp          = [this](const transport::RtpDatagram& dg)       { on_rtp(dg); };
        sink_cb.on_rtcp         = [](auto&, auto&, auto&, auto&) {};
        sink_cb.on_error        = [this](mc_status_t c, std::string m)            { on_error(c, std::move(m)); };
        sink_cb.on_network_down = [] {};
        sink_cb.on_network_up   = [] {};
        transport->set_sink(std::move(sink_cb));

        const std::string user = opt.username ? opt.username : "";
        const std::string pass = opt.password ? opt.password : "";
        const uint32_t connect_to = opt.connect_timeout_ms ? opt.connect_timeout_ms : kDefaultConnectTimeoutMs;
        const uint32_t read_to    = opt.read_timeout_ms    ? opt.read_timeout_ms    : kDefaultReadTimeoutMs;
        const uint32_t ka_ms      = opt.keepalive_interval_ms ? opt.keepalive_interval_ms : kDefaultKeepaliveMs;

        if (mc_status_t s = transport->open(opt.url, user, pass, connect_to, read_to, ka_ms);
            s != MC_OK) {
            emit_error(s, "transport open failed");
            return s;
        }
        return MC_OK;
    }

    void close_unlocked() noexcept {
        // 注意：调用此函数前 caller 必须已无锁 join audio_start_thread（见 Controller::close），
        // 否则 audio start 子线程对 audio_render / codec_audio / depack_aac 的写入会与下方 reset 竞争。
        if (transport) {
            transport->close();
            transport.reset();
        }
        // 拆解码-渲染管线（codec 先停，避免 emit 到已销毁 gate）。
        if (codec_video) {
            codec_video->stop();
            codec_video.reset();
        }
        if (codec_dxva) {
            codec_dxva->stop();
            codec_dxva.reset();
        }
        if (codec_libcodec) {
            codec_libcodec->stop();
            codec_libcodec.reset();
        }
        gate.reset();
        if (render) {
            render->stop();
            render.reset();
        }
        // 音频先停 codec（drain 不会再 emit）→ 渲染（停 worker）→ depack。
        if (codec_audio)  { codec_audio->stop(); codec_audio.reset(); }
        if (audio_render) { audio_render->stop(); audio_render.reset(); }
        depack_aac.reset();
        // 先停 jitter buffer(它持 EmitFn 闭包引用 depack;先释放它再 release depack)。
        jitter_video.reset();
        depack_h264.reset();
        depack_h265.reset();
        d3d_device.Reset();
        close_dump_file();

        const mc_state_t prev = state.exchange(MC_STATE_CLOSED, std::memory_order_acq_rel);
        if (prev != MC_STATE_CLOSED) {
            emit_state_change(prev, MC_STATE_CLOSED);
        }
    }
};

Controller::Controller() : impl_{std::make_unique<Impl>()} {}
Controller::~Controller() { close(); }

void Controller::set_event_sink(EventSink sink) noexcept {
    std::scoped_lock lk{impl_->mu};
    impl_->sink = std::move(sink);
}

mc_status_t Controller::open(const mc_open_options_t& options) noexcept {
    // Phase 8-E: HDCM batch detect 异步触发,不阻塞 mc_open 首帧路径(性能规范 §3)。
    // adapter_vendor_id=0 让 detector 把所有类别 A 都标 unavailable_on_this_sku;
    // 待 set_render_target 后 d3d_device 可用时再补做一次精确 detect。
    std::thread([this] {
        (void)hdcm::detect_all_components(/*adapter_vendor_id=*/0);
    }).detach();

    std::scoped_lock lk{impl_->mu};
    return impl_->open_unlocked(options);
}

void Controller::close() noexcept {
    // 阶段一：无锁 join 异步音频启动线程（可能正在 WASAPI Initialize 中），避免持 mu 时与之死锁。
    std::thread t;
    {
        std::scoped_lock lk{impl_->mu};
        t = std::move(impl_->audio_start_thread);
    }
    if (t.joinable()) t.join();
    // 阶段二：常规 cleanup。
    std::scoped_lock lk{impl_->mu};
    impl_->close_unlocked();
}

mc_status_t Controller::set_render_target(HWND hwnd) noexcept {
    std::scoped_lock lk{impl_->mu};
    impl_->render_hwnd = hwnd;
    return MC_OK;
}

mc_status_t Controller::get_state(mc_state_t& out) const noexcept {
    out = impl_->state.load(std::memory_order_acquire);
    return MC_OK;
}

mc_status_t Controller::get_stats(mc_stats_t& inout) const noexcept {
    if (inout.struct_size < sizeof(mc_stats_t)) {
        return MC_ERR_INVALID_ARG;
    }
    inout.rtp_packets_received  = impl_->rtp_packets.load(std::memory_order_relaxed);
    inout.video_frames_decoded  = impl_->au_count.load(std::memory_order_relaxed);
    if (impl_->gate)   impl_->gate->fill_stats(inout);
    if (impl_->render) {
        inout.active_render_profile        = impl_->render->active_profile();
        inout.active_present_mode          = impl_->render->active_present_mode();
        inout.present_count                = impl_->render->present_count();
        inout.present_skipped_by_watchdog  = impl_->render->skip_count();
    }
    return MC_OK;
}

mc_status_t Controller::get_stream_info(mc_stream_info_t& inout) const noexcept {
    if (inout.struct_size < sizeof(mc_stream_info_t)) {
        return MC_ERR_INVALID_ARG;
    }
    inout.video_codec        = impl_->video_codec;
    inout.video_decoder_kind = impl_->decoder_kind;
    inout.active_protocol    =
        (impl_->options.protocol_hint == MC_PROTOCOL_RTSP_TCP)
            ? MC_PROTOCOL_RTSP_TCP : MC_PROTOCOL_RTSP_UDP;

    inout.gpu_kind = MC_GPU_KIND_UNKNOWN;
    inout.gpu_description[0] = '\0';
    if (const auto* picked_caps = impl_->caps_probe.find_by_luid(impl_->adapter_pick.luid)) {
        inout.gpu_kind = classify_gpu(*picked_caps);
        copy_adapter_description(inout.gpu_description, picked_caps->description);
    }
    return MC_OK;
}

mc_status_t Controller::tick_ui() noexcept {
    if (!impl_->render) return MC_OK;     // render 未启动（empty 态）—— 后续会建好再 tick
    // 把最新 stats 推到 UI（playing stage 的 latency / dropped / fps 显示）
    if (auto* ui = impl_->render->ui_overlay()) {
        mc_stats_t s{};
        s.struct_size = sizeof(s);
        s.struct_version = MC_STATS_VERSION;
        (void)get_stats(s);
        ui->set_stats(s);
    }
    impl_->render->tick_ui();

    // ADR-015 sync software MFT silent fail 自动降档(IoT LTSC 缺核心解码 dll 时,
    // sync MFT 仍激活但 ProcessOutput 输出全 0 NV12 → render 显示深绿屏)。
    // codec_mft_video 启动期 readback Y plane 检测,confirmed=true 时一次性降到 tier 4。
    if (!impl_->tier3_demoted_to_tier4 &&
        impl_->codec_video &&
        impl_->codec_video->silent_fail_confirmed()) {
        std::scoped_lock lk{impl_->mu};
        if (!impl_->tier3_demoted_to_tier4 && impl_->codec_video) {
            MCP_LOG_ERROR("Controller: tier3 sync software MFT silent fail confirmed, "
                          "降档到 tier4 mc-libcodec");
            auto& reg = pal::metric::Registry::instance();
            reg.counter("mc.probe.tier_skip_reason.tier3.silent_fail_runtime").inc();
            impl_->codec_video->stop();
            impl_->codec_video.reset();
            if (impl_->gate) impl_->gate->reset();

            media::CodecLibcodecVideo::Config lcfg;
            lcfg.codec  = impl_->video_codec;
            lcfg.device = impl_->d3d_device;
            lcfg.emit   = [this](media::VideoFrame&& f) { impl_->on_decoded_frame(std::move(f)); };
            impl_->codec_libcodec = std::make_unique<media::CodecLibcodecVideo>(std::move(lcfg));
            if (mc_status_t s = impl_->codec_libcodec->start(); s == MC_OK) {
                impl_->decoder_kind = MC_DECODER_LIBCODEC;
                reg.gauge("mc.decoder.kind").set(MC_DECODER_LIBCODEC);
                reg.gauge("mc.probe.tier_selected").set(4);
                MCP_LOG_INFO("Controller: tier4 mc-libcodec active (downgraded from tier3)");
            } else {
                MCP_LOGF(pal::LogLevel::error,
                         "Controller: tier4 libcodec start failed status=%d "
                         "(after tier3 silent fail) → 无可用解码档,显示冻结",
                         static_cast<int>(s));
                impl_->codec_libcodec.reset();
                impl_->decoder_kind = MC_DECODER_NONE;
                reg.gauge("mc.decoder.kind").set(MC_DECODER_NONE);
            }
            impl_->tier3_demoted_to_tier4 = true;
        }
    }

    // ADR-014 / ADD §5.10.5：Present Epoch 陈旧区域 watchdog 已由 T5 渲染线程自驱
    // （render_thread_loop 内每 ~frame_period 唤醒一次），main 不再触发。
    return MC_OK;
}

mc_status_t Controller::set_show_stats(bool show) noexcept {
    if (impl_->render && impl_->render->ui_overlay()) {
        impl_->render->ui_overlay()->set_show_stats(show);
    }
    return MC_OK;
}

mc_status_t Controller::set_show_add_modal(bool show) noexcept {
    if (impl_->render && impl_->render->ui_overlay()) {
        impl_->render->ui_overlay()->set_show_add_modal(show);
    }
    return MC_OK;
}

}  // namespace mcp::controller
