#include "controller/controller.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3d11_4.h>          // ID3D11Multithread（同 device 上的并发保护开关）
#include <dxgi1_6.h>
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
#include "media/codec_dxva_video.h"
#include "media/codec_g711.h"
#include "media/codec_libcodec.h"
#include "media/codec_mft_audio.h"
#include "media/codec_mft_video.h"
#include "media/color_meta.h"
#include "media/depack_aac.h"
#include "media/depack_h264.h"
#include "media/depack_h265.h"
#include "media/frame_validity_gate.h"
#include "media/render_d3d11.h"
#include "media/ui_overlay.h"
#include "pal/dxgi_caps_probe.h"
#include "pal/log.h"
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
    std::unique_ptr<media::DepackH264>            depack_h264;
    std::unique_ptr<media::DepackH265>            depack_h265;
    std::unique_ptr<media::CodecMftVideo>         codec_video;
    std::unique_ptr<media::CodecDxvaVideo>        codec_dxva;
    std::unique_ptr<media::CodecLibcodecVideo>    codec_libcodec;
    std::unique_ptr<media::FrameValidityGate>     gate;
    std::unique_ptr<media::RenderD3d11>           render;

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

    // RTCP 上行：本端 sender SSRC（每会话随机一次） + 视频远端 SSRC（首个视频 RTP 学到）。
    // pli_sent 走 atomic_flag — 只允许首帧到达时发一次 PLI，触发摄像头立即推 IDR。
    uint32_t                                   local_sender_ssrc = 0;
    std::atomic_flag                           pli_sent = ATOMIC_FLAG_INIT;

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
        if (gate) gate->admit(f);
    }

    void on_admitted(const media::VideoFrame& f) noexcept {
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
        MCP_LOGF(pal::LogLevel::info,
                 "Controller: codec policy: low_latency=%d (default optimistic; runtime PTS-inversion fallback) "
                 "color(pri=%d range=%d mat=%d) h_hint=%u",
                 decoded_no_b_frames ? 1 : 0,
                 static_cast<int>(resolved_color.primaries),
                 static_cast<int>(resolved_color.range),
                 static_cast<int>(resolved_color.matrix),
                 height_hint);
    }

    // 构造 RFC 4585 §3.1 兼容的复合包（RR + SDES + PLI）并下发到 transport。
    // 仅 RTSP-TCP interleaved 路径会真的把 RTCP 送回摄像头；UDP / WHEP transport 当前
    // send_rtcp 返回 MC_ERR_UNSUPPORTED，调用方静默丢弃。
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
        MCP_LOGF(pal::LogLevel::info,
                 "Controller: sent video PLI media_ssrc=0x%08X status=%d",
                 media_ssrc, static_cast<int>(s));
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

        // 解码器选择链：MFT（首选）→ DXVA-direct（HEVC Extension 缺席兜底）→ libcodec（纯软解）。
        media::CodecMftVideo::Config ccfg;
        ccfg.codec  = video_codec;
        ccfg.device = d3d_device;
        ccfg.prefer_low_latency = decoded_no_b_frames;     // ADD §5.6.4 B-Frame Policy
        ccfg.color  = resolved_color;                       // ADD §5.12 三级兜底结果
        ccfg.emit   = [this](media::VideoFrame&& f) { on_decoded_frame(std::move(f)); };
        codec_video = std::make_unique<media::CodecMftVideo>(std::move(ccfg));
        if (mc_status_t s = codec_video->start({}); s == MC_OK) {
            decoder_kind = MC_DECODER_MFT_HARDWARE;
            return MC_OK;
        } else {
            MCP_LOGF(pal::LogLevel::warn,
                     "Controller: MFT codec start failed status=%d", static_cast<int>(s));
            codec_video.reset();
        }

        // DXVA-direct 仅对 HEVC 有意义（H.264 已被 MFT 覆盖；MFT 失败说明系统层全坏）。
        if (video_codec == MC_VIDEO_CODEC_H265) {
            media::CodecDxvaVideo::Config dcfg;
            dcfg.codec  = video_codec;
            dcfg.device = d3d_device;
            dcfg.emit   = [this](media::VideoFrame&& f) { on_decoded_frame(std::move(f)); };
            codec_dxva  = std::make_unique<media::CodecDxvaVideo>(std::move(dcfg));
            if (mc_status_t s = codec_dxva->start(); s == MC_OK) {
                MCP_LOGF(pal::LogLevel::info,
                         "Controller: HEVC running on DXVA-direct (no HEVC Extension required)");
                decoder_kind = MC_DECODER_MFT_HARDWARE;     // 共用枚举：仍是硬件解码
                return MC_OK;
            } else {
                MCP_LOGF(pal::LogLevel::warn,
                         "Controller: DXVA-direct start failed status=%d", static_cast<int>(s));
                codec_dxva.reset();
            }
        }

        if (!options.allow_software_decode) {
            return MC_ERR_NO_HARDWARE;
        }

        media::CodecLibcodecVideo::Config lcfg;
        lcfg.codec  = video_codec;
        lcfg.device = d3d_device;
        lcfg.emit   = [this](media::VideoFrame&& f) { on_decoded_frame(std::move(f)); };
        codec_libcodec = std::make_unique<media::CodecLibcodecVideo>(std::move(lcfg));
        if (mc_status_t s = codec_libcodec->start(); s != MC_OK) {
            MCP_LOGF(pal::LogLevel::error,
                     "Controller: libcodec start failed status=%d", static_cast<int>(s));
            codec_libcodec.reset();
            decoder_kind = MC_DECODER_NONE;
            return s;
        }
        decoder_kind = MC_DECODER_LIBCODEC;
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
        e.stream_info.active_protocol    = MC_PROTOCOL_RTSP_UDP;

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

    void on_rtp(const transport::RtpDatagram& dg) noexcept {
        rtp_packets.fetch_add(1, std::memory_order_relaxed);
        if (dg.bytes.size() < 12) return;

        const uint8_t* p = dg.bytes.data();
        const bool marker  = (p[1] & 0x80) != 0;
        const uint32_t ts  = (static_cast<uint32_t>(p[4]) << 24) |
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

        if (dg.kind == transport::MediaKind::video) {
            // 视频时钟固定 90 kHz：us = ts * 1e6 / 90000 = ts * 100 / 9
            const int64_t pts_us = static_cast<int64_t>(ts) * 100 / 9;
            // 首个视频 RTP 触发 PLI：摄像头收到立即下发 IDR，缩短首帧延时。
            // 多数低延时摄像头 GOP 几秒，进流时若位于 GOP 中段，无 PLI 时要等下一个
            // 周期性 IDR；PLI 把这段空窗压到一帧 RTT 之内（RFC 4585 §6.3.1）。
            if (!pli_sent.test_and_set(std::memory_order_acq_rel)) {
                const uint32_t media_ssrc = (static_cast<uint32_t>(p[8])  << 24) |
                                              (static_cast<uint32_t>(p[9])  << 16) |
                                              (static_cast<uint32_t>(p[10]) <<  8) |
                                               static_cast<uint32_t>(p[11]);
                send_video_pli(media_ssrc);
            }
            if (depack_h264)      depack_h264->on_rtp(pts_us, marker, payload, dg.arrival_qpc_ns);
            else if (depack_h265) depack_h265->on_rtp(pts_us, marker, payload, dg.arrival_qpc_ns);
        } else if (dg.kind == transport::MediaKind::audio) {
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
        pli_sent.clear(std::memory_order_release);

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
