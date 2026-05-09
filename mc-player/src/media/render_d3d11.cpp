#include "media/render_d3d11.h"

#include <d3d11_4.h>          // ID3D11Multithread（codec 也提交命令到同 immediate ctx）
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

#include "media/ui_overlay.h"
#include "media/video_clock.h"
#include "pal/clock.h"
#include "pal/error.h"
#include "pal/log.h"
#include "pal/metric.h"
#include "pal/raii.h"
#include "pal/thread.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace mcp::media {

namespace {

// 全屏三角形 — 顶点 ID 直接生成 NDC 坐标，无需 vertex buffer。
constexpr char kVS[] = R"(
struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VS_OUT main(uint id : SV_VertexID) {
    VS_OUT o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";

// NV12 → RGB（BT.709 limited，PC 范围归一化）。Y 平面 R8 SRV，UV 平面 R8G8 SRV。
constexpr char kPS[] = R"(
Texture2D<float>  YPlane  : register(t0);
Texture2D<float2> UVPlane : register(t1);
SamplerState      Samp    : register(s0);

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float  y  = YPlane.Sample(Samp, uv).r;
    float2 cc = UVPlane.Sample(Samp, uv).rg;
    float u = cc.x - 0.5;
    float v = cc.y - 0.5;
    // BT.709 limited → full range
    float yn = (y - 16.0/255.0) * (255.0/219.0);
    float un = u * (255.0/224.0);
    float vn = v * (255.0/224.0);
    float r = yn + 1.5748 * vn;
    float g = yn - 0.1873 * un - 0.4681 * vn;
    float b = yn + 1.8556 * un;
    return float4(saturate(float3(r, g, b)), 1);
}
)";

// Watchdog 触发阈值：N × frame_period 未推进则强制 redraw（ADD §5.10.5）。
// 3 来自架构文档「N 默认 3 倍」（PresentEpoch::tick 文档）。
constexpr int kWatchdogPeriodsThreshold = 3;

// 显示器实际刷新率 → frame period（ns）。
// 走 HWND→HMONITOR→MONITORINFOEX→EnumDisplaySettings；失败时按 DEVMODE 默认值兜底。
int64_t query_frame_period_ns(HWND hwnd) noexcept {
    if (!hwnd) return 0;
    HMONITOR mon = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!mon) return 0;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(mon, &mi)) return 0;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!::EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0;
    if (dm.dmDisplayFrequency <= 1) return 0;     // 0/1 表示「default / unspecified」
    return 1'000'000'000LL / static_cast<int64_t>(dm.dmDisplayFrequency);
}

}  // namespace

struct RenderD3d11::Impl {
    Config                                  cfg;
    std::unique_ptr<SwapChain>              swap_chain;
    std::unique_ptr<DcompRoot>              dcomp;
    std::unique_ptr<PresentEpoch>           epoch;
    std::unique_ptr<UiOverlay>              ui;

    // ADD §3.3 T5 渲染线程：独占 swap_chain Present + DCOMP commit 调用方。
    // 解码线程（T4 / T2）通过 post_frame() 投递最新帧 + signal wake_event 唤醒；
    // T5 自驱 watchdog（无需 main 周期 tick）。
    //
    // last_good / has_last_good 仅 T5 写入与读取，无需锁。
    // 投递队列 pending_queue 由 codec 线程写、T5 读;codec 在 GOP 边界 reorder buffer
    // drain 时会 burst emit 多帧(典型 IBBBP 跨 GOP:单次 push 触发 emit P_old + emit
    // IDR_new 两帧),旧版单 slot drop-old 会丢失 P_old → 「跳跃」感。
    //
    // 改为容量 8 的 FIFO 小 queue:
    //   - codec 端 push_back 入队;若满则 drop_oldest(最 stale 的 PTS)。
    //   - T5 端 pop_front 按 PTS 顺序消费(reorder 已保证 emit 端 PTS 单调)。
    // 容量 8 = ~270ms @30fps,覆盖 GOP 边界 burst(典型 1-3 帧)+ short-burst 网络重传。
    // 对齐 VLC vout queue 风格但不做 PTS-vs-master_clock 调度(下个 plan)。
    VideoFrame                              last_good;
    bool                                    has_last_good = false;
    std::atomic<int64_t>                    frame_period_ns{0};   // 0 = 未知（watchdog 走 fallback）

    // VLC vlc_clock 完整对齐 — video 当 master(无 audio path),
    // master_update 收 EMA coeff + 重算 offset,convert_to_system 算 deadline,
    // wait_until 用 cv.wait_for 可被 wake/stop 中断。详见 video_clock.h 文件头注释。
    VideoClock                              video_clock;

    // Phase 4 — vsync frame doubling estimator。
    // 嗅探 source_period_avg(EMA last 16 PTS deltas),除 display vsync_period 算
    // SyncInterval。30fps source / 60Hz display = 2,Present(SyncInterval=2) 让 GPU
    // 每 2 vsync 翻 backbuffer = 33.33ms,严格锁帧消除 vsync drift "短-短-长"。
    struct SyncEst {
        std::atomic<int64_t> source_period_avg_ns{0};
        std::atomic<int>     sample_count{0};
        int64_t              last_pts_us = INT64_MIN;     // 仅 T5 写,无锁

        void observe(int64_t pts_us) noexcept {
            if (last_pts_us != INT64_MIN) {
                const int64_t delta_us = pts_us - last_pts_us;
                // 过滤 reorder noise(B 帧 pts 倒退)+ wrap 异常,只取合理 frame interval。
                // 10-100ms 覆盖 10-100 fps source。
                if (delta_us > 10'000 && delta_us < 100'000) {
                    const int64_t cur = source_period_avg_ns.load(std::memory_order_relaxed);
                    const int     cnt = sample_count.load(std::memory_order_relaxed);
                    const int64_t newv = (cnt == 0)
                        ? (delta_us * 1000)
                        : (cur * 15 + delta_us * 1000) / 16;     // EMA range=16
                    source_period_avg_ns.store(newv, std::memory_order_relaxed);
                    if (cnt < 16) sample_count.store(cnt + 1, std::memory_order_relaxed);
                }
            }
            last_pts_us = pts_us;
        }

        /// 0 = 未稳定 / 用 SyncInterval=0(立即 Present + tearing fallback)。
        /// 1-4 = 用 SyncInterval=N 让 GPU 锁 N vsync 周期。
        [[nodiscard]] uint32_t compute_sync_interval(int64_t vsync_period_ns) const noexcept {
            const int cnt = sample_count.load(std::memory_order_relaxed);
            if (cnt < 8 || vsync_period_ns <= 0) return 0;     // 未稳定
            const int64_t period = source_period_avg_ns.load(std::memory_order_relaxed);
            const double ratio = static_cast<double>(period) / static_cast<double>(vsync_period_ns);
            // ratio < 0.7 source 比 vsync 快太多,不能 frame doubling,fallback 0。
            if (ratio < 0.7) return 0;
            const int interval = static_cast<int>(ratio + 0.5);
            return static_cast<uint32_t>(std::clamp(interval, 1, 4));
        }
    };
    SyncEst                                 sync_est;

    std::thread                             render_thread;
    std::atomic<bool>                       render_stop{false};
    pal::HandleGuard                        wake_event;             // auto-reset
    // Phase 4 vsync interval=2 模式:Present 内部 block 直到 N vsync,T5 处理速度
    // 严格锁定 GPU vsync。queue cap=8 限制 backlog,startup burst > 8 帧的丢帧用
    // drop_oldest 容忍(对画面无感,因 burst 帧时间几乎相同)。
    static constexpr std::size_t            kPendingQueueCap = 8;
    std::mutex                              pending_mu;
    std::deque<VideoFrame>                  pending_queue;
    std::atomic<uint64_t>                   pending_drops{0};       // 队列满时 drop_oldest 计数

    ComPtr<ID3D11DeviceContext>             ctx;
    ComPtr<ID3D11RenderTargetView>          rtv;
    ComPtr<ID3D11VertexShader>              vs;
    ComPtr<ID3D11PixelShader>               ps;
    ComPtr<ID3D11SamplerState>              samp;
    ComPtr<ID3D11RasterizerState>           raster;

    // SRV 缓存（P1-8）：codec 自管 NV12 array pool 的 slice 是有限集。
    // 零拷贝路径下 frame.dxva_texture = codec dpb_tex，dpb_size 上限 24（H.264）/ 8（HEVC），
    // 加 last_good anchor 缓冲取 32 避免 slice 模冲突重建。
    // 按 (texture* + slice) 缓存 Y/UV SRV；pool 重建（dxva_texture 指针变更）整组失效重建。
    static constexpr int kSrvCacheSize = 32;
    struct SrvSlot {
        ID3D11Texture2D*                  tex   = nullptr;     // 仅作 identity，不持引用
        uint32_t                          slice = 0;
        ComPtr<ID3D11ShaderResourceView>  y;
        ComPtr<ID3D11ShaderResourceView>  uv;
    };
    SrvSlot                                 srv_cache[kSrvCacheSize];
    void invalidate_srv_cache() noexcept {
        for (auto& s : srv_cache) { s = {}; }
    }

    bool                                    pipeline_ready = false;
    std::atomic<uint64_t>                   presents{0};

    // 端到端延时探针（P0-4）：frame.arrival_qpc_ns → present_now_ns 的累计 ring，
    // 周期性输出 P50/P95/P99。仅 T5 单线程访问 ring/head/filled/last_log_ns；
    // samples_total/samples_zero 用 atomic 是因 stats 接口可能从 UI 线程读。
    struct LatencyProbe {
        static constexpr std::size_t kCap            = 256;
        static constexpr int64_t     kLogIntervalNs  = 5'000'000'000LL;   // 5 秒
        std::array<int64_t, kCap>    ring{};
        std::size_t                  head            = 0;
        std::size_t                  filled          = 0;
        int64_t                      last_log_ns     = 0;
        std::atomic<uint64_t>        samples_total{0};
        std::atomic<uint64_t>        samples_zero{0};

        void record(int64_t e2e_ns) noexcept {
            ring[head] = e2e_ns;
            head       = (head + 1) % kCap;
            if (filled < kCap) ++filled;
        }
        void log_if_due(int64_t now_ns) noexcept {
            if (filled < 8) return;
            if (last_log_ns != 0 && now_ns - last_log_ns < kLogIntervalNs) return;
            last_log_ns = now_ns;
            std::array<int64_t, kCap> sorted{};
            for (std::size_t i = 0; i < filled; ++i) sorted[i] = ring[i];
            std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(filled));
            const auto pct = [&](double p) noexcept -> int64_t {
                const auto idx = static_cast<std::size_t>(p * (filled - 1));
                return sorted[idx];
            };
            const int64_t  p50   = pct(0.50);
            const int64_t  p95   = pct(0.95);
            const int64_t  p99   = pct(0.99);
            const uint64_t total = samples_total.load(std::memory_order_relaxed);
            const uint64_t zero  = samples_zero.load(std::memory_order_relaxed);
            MCP_LOGF(pal::LogLevel::info,
                     "RenderD3d11: e2e latency P50=%lld us P95=%lld us P99=%lld us "
                     "(window=%zu total=%llu untraceable=%llu)",
                     static_cast<long long>(p50 / 1000),
                     static_cast<long long>(p95 / 1000),
                     static_cast<long long>(p99 / 1000),
                     filled,
                     static_cast<unsigned long long>(total),
                     static_cast<unsigned long long>(zero));
        }
    };
    LatencyProbe                            latency_probe;

    HRESULT compile_shaders() noexcept {
        ComPtr<ID3DBlob> blob, err;
        HRESULT hr = ::D3DCompile(kVS, sizeof(kVS) - 1, "vs", nullptr, nullptr,
                                    "main", "vs_4_0", 0, 0, &blob, &err);
        if (FAILED(hr)) {
            MCP_LOGF(pal::LogLevel::error,
                     "RenderD3d11: VS compile failed: %s",
                     err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            return hr;
        }
        hr = cfg.device->CreateVertexShader(blob->GetBufferPointer(),
                                              blob->GetBufferSize(), nullptr, &vs);
        if (FAILED(hr)) return hr;

        blob.Reset();
        err.Reset();
        hr = ::D3DCompile(kPS, sizeof(kPS) - 1, "ps", nullptr, nullptr,
                            "main", "ps_4_0", 0, 0, &blob, &err);
        if (FAILED(hr)) {
            MCP_LOGF(pal::LogLevel::error,
                     "RenderD3d11: PS compile failed: %s",
                     err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            return hr;
        }
        return cfg.device->CreatePixelShader(blob->GetBufferPointer(),
                                              blob->GetBufferSize(), nullptr, &ps);
    }

    HRESULT create_pipeline_state() noexcept {
        HRESULT hr = compile_shaders();
        if (FAILED(hr)) return hr;

        D3D11_SAMPLER_DESC sd{};
        sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD   = D3D11_FLOAT32_MAX;
        cfg.device->CreateSamplerState(&sd, &samp);

        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID;
        rs.CullMode = D3D11_CULL_NONE;
        cfg.device->CreateRasterizerState(&rs, &raster);
        return S_OK;
    }

    HRESULT ensure_rtv() noexcept {
        if (rtv) return S_OK;
        ComPtr<ID3D11Texture2D> bb = swap_chain->back_buffer();
        if (!bb) return E_FAIL;
        HRESULT hr = cfg.device->CreateRenderTargetView(bb.Get(), nullptr, &rtv);
        if (FAILED(hr)) return hr;
        // D2D RT 跟 D3D11 RTV 共绑同一 backbuffer surface；ResizeBuffers 时一起重建。
        if (ui) (void)ui->bind_backbuffer(bb);
        return S_OK;
    }

    void clear_views() noexcept {
        if (ui) ui->unbind_backbuffer();
        rtv.Reset();
        // back buffer 重建时不必显式清 SRV 缓存（缓存键是 codec NV12 texture，与 swap chain 无关），
        // 但 device-lost 路径会重建 codec pool → 下次 render_locked 命中失败自然刷新。保险起见显式清。
        invalidate_srv_cache();
    }

    static void clear_views_thunk(void* user) noexcept {
        if (auto* self = static_cast<Impl*>(user)) self->clear_views();
    }

    // T5 主循环：独占 swap_chain Present + DCOMP commit；自驱 watchdog；
    // 由 post_frame() 唤醒接收最新帧，无帧期间 ~frame_period 节拍 redraw（动画 + 陈旧防御）。
    void render_thread_loop() noexcept;

    // 以下两个 *_locked 命名沿用历史，但 T5 单线程化后不再持外部锁。
    // 仅画 UI（无视频帧）：empty/connecting/error stage + freeze 期使用。
    void render_ui_only_locked() noexcept {
        if (!swap_chain || !pipeline_ready) return;

        // P0-2: wait 在 record 之前。配合 SetMaximumFrameLatency(1)，让 record 工作
        // 落在「DXGI 释放新帧」之后的窗口，避免占用前一帧 vsync 边界时间（DXGI 推荐
        // 顺序：wait → record → Present）。timeout 50ms 防显示器异常永久 block。
        (void)swap_chain->wait_for_frame_latency(50);

        if (FAILED(ensure_rtv())) return;

        ComPtr<ID3D11Texture2D> bb = swap_chain->back_buffer();
        D3D11_TEXTURE2D_DESC bb_desc{};
        if (bb) bb->GetDesc(&bb_desc);
        D3D11_VIEWPORT vp{};
        vp.Width    = static_cast<float>(bb_desc.Width  ? bb_desc.Width  : 1);
        vp.Height   = static_cast<float>(bb_desc.Height ? bb_desc.Height : 1);
        vp.MaxDepth = 1.0f;

        const FLOAT clear[4] = {0, 0, 0, 1};
        ID3D11RenderTargetView* rtvs[] = { rtv.Get() };
        ctx->ClearRenderTargetView(rtv.Get(), clear);
        ctx->OMSetRenderTargets(1, rtvs, nullptr);
        ctx->RSSetViewports(1, &vp);

        // 解绑 RTV 让 D2D 接管
        ID3D11RenderTargetView* null_rtvs[] = { nullptr };
        ctx->OMSetRenderTargets(1, null_rtvs, nullptr);

        if (ui) ui->render();

        const bool tearing =
            (swap_chain->active_present_mode() == MC_PRESENT_MODE_TEARING);
        // UI-only redraw(no video frame)用 SyncInterval=1 等 1 vsync 即可,无 frame doubling。
        // 避免 watchdog redraw 阻塞过久。
        // ADD §5.10.5 epoch 不变量: video Present 与 DCOMP commit 同 epoch 配对,
        // Present 失败时不应调 on_presented。
        if (swap_chain->present(tearing ? 0u : 1u, tearing) == MC_OK) {
            presents.fetch_add(1, std::memory_order_relaxed);
            if (epoch) epoch->on_presented(pal::Clock::now_ns());
        }
    }

    void render_locked(const VideoFrame& frame) noexcept {
        if (!swap_chain || !pipeline_ready) return;

        // P0-2: wait 在 record 之前（DXGI 推荐顺序：wait → record → Present），
        // 让 record 工作落在「DXGI 释放新帧」之后的窗口；timeout 50ms 防永久 block。
        // force_redraw 回调（watchdog）也走 render_locked，自然继承新顺序。
        (void)swap_chain->wait_for_frame_latency(50);

        if (FAILED(ensure_rtv())) return;
        if (!frame.dxva_texture) return;

        // 创建 NV12 双 SRV（Y=R8，UV=R8G8）。
        // AMD 独显某些 driver 对 NV12 + BIND_DECODER + ArraySize>1 的 SRV 创建报错；
        // 出错时降级到 TEXTURE2D（ArraySize=1）路径，由 codec 端把帧拷到单 slice 纹理。
        D3D11_TEXTURE2D_DESC tex_desc{};
        frame.dxva_texture->GetDesc(&tex_desc);
        const bool is_array = tex_desc.ArraySize > 1;

        // SRV 缓存：(texture identity, slice) 命中即复用，未命中则按 LRU 槽（slice 索引模容量）
        // 创建并填入。codec pool 重建时 dxva_texture 指针变化，下一帧自然驱动重填。
        ID3D11Texture2D* const tex_ptr = frame.dxva_texture.Get();
        const uint32_t slice = frame.dxva_array_slice;
        SrvSlot& slot = srv_cache[slice % kSrvCacheSize];
        ComPtr<ID3D11ShaderResourceView> y_srv;
        ComPtr<ID3D11ShaderResourceView> uv_srv;
        if (slot.tex == tex_ptr && slot.slice == slice && slot.y && slot.uv) {
            y_srv  = slot.y;
            uv_srv = slot.uv;
        } else {
            // VLC d3d11_fmt.cpp::D3D11_AllocateResourceView 标准路径:用旧版
            // CreateShaderResourceView,按 SRV.Format (R8_UNORM / R8G8_UNORM) 让 D3D11
            // runtime 自动映射到 NV12 的 plane 0 / plane 1。不用 SRV1.PlaneSlice —
            // Intel UHD 730 + Win11 IoT LTSC driver 对 NV12 array texture + SRV1
            // PlaneSlice=1 处理 buggy:实测 PlaneSlice=1 取到的是 plane 0 (Y) 数据,
            // shader 当 UV 用 → r/g/b 都基于 Y 算,大面积紫色 + 灰色错位渐变(2026-05-08
            // root cause)。VLC 实测 UHD 730 + Win11 上工作 — 这条 driver path 稳定。
            D3D11_SHADER_RESOURCE_VIEW_DESC y_desc{};
            y_desc.Format = DXGI_FORMAT_R8_UNORM;
            if (is_array) {
                y_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                y_desc.Texture2DArray.MostDetailedMip = 0;
                y_desc.Texture2DArray.MipLevels       = 1;
                y_desc.Texture2DArray.FirstArraySlice = slice;
                y_desc.Texture2DArray.ArraySize       = 1;
            } else {
                y_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                y_desc.Texture2D.MostDetailedMip = 0;
                y_desc.Texture2D.MipLevels       = 1;
            }
            HRESULT hy = cfg.device->CreateShaderResourceView(tex_ptr, &y_desc, &y_srv);
            if (FAILED(hy)) {
                MCP_LOGF(pal::LogLevel::error,
                         "RenderD3d11: NV12 Y SRV hr=0x%08lX (%ux%u arr=%u slice=%u)",
                         hy, tex_desc.Width, tex_desc.Height, tex_desc.ArraySize, slice);
                return;
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC uv_desc = y_desc;
            uv_desc.Format = DXGI_FORMAT_R8G8_UNORM;
            HRESULT hu = cfg.device->CreateShaderResourceView(tex_ptr, &uv_desc, &uv_srv);
            if (FAILED(hu)) {
                MCP_LOGF(pal::LogLevel::error,
                         "RenderD3d11: NV12 UV SRV hr=0x%08lX (slice=%u)", hu, slice);
                return;
            }
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true, std::memory_order_relaxed)) {
                MCP_LOG_INFO("RenderD3d11: NV12 SRV via Format (R8/R8G8,VLC-style,driver auto plane)");
            }
            slot.tex   = tex_ptr;
            slot.slice = slice;
            slot.y     = y_srv;
            slot.uv    = uv_srv;
        }

        {
            static std::atomic<uint64_t> n{0};
            if (n.fetch_add(1, std::memory_order_relaxed) == 0) {
                MCP_LOGF(pal::LogLevel::info,
                         "RenderD3d11: first frame render (texSize=%ux%u arr=%u slice=%u src=%d)",
                         tex_desc.Width, tex_desc.Height, tex_desc.ArraySize,
                         frame.dxva_array_slice, static_cast<int>(frame.source));
            }
        }

        D3D11_VIEWPORT vp{};
        DXGI_SWAP_CHAIN_DESC1 sc_desc{};
        // back buffer 实际尺寸；Resize 暂未实装，跟着创建尺寸走。
        ComPtr<ID3D11Texture2D> bb = swap_chain->back_buffer();
        D3D11_TEXTURE2D_DESC bb_desc{};
        if (bb) bb->GetDesc(&bb_desc);
        vp.Width  = static_cast<float>(bb_desc.Width  ? bb_desc.Width  : 1);
        vp.Height = static_cast<float>(bb_desc.Height ? bb_desc.Height : 1);
        vp.MaxDepth = 1.0f;

        const FLOAT clear[4] = {0, 0, 0, 1};
        ID3D11RenderTargetView* rtvs[] = { rtv.Get() };
        ID3D11ShaderResourceView* srvs[] = { y_srv.Get(), uv_srv.Get() };
        ID3D11SamplerState* samps[] = { samp.Get() };

        ctx->ClearRenderTargetView(rtv.Get(), clear);
        ctx->OMSetRenderTargets(1, rtvs, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(raster.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->PSSetSamplers(0, 1, samps);
        ctx->Draw(3, 0);

        // 解绑 SRV 防止后续 RTV 重新 bind 时验证警告。
        ID3D11ShaderResourceView* nulls[] = {nullptr, nullptr};
        ctx->PSSetShaderResources(0, 2, nulls);

        // 解绑 D3D11 RTV 让 D2D 接管同一 backbuffer surface（ADD §5.10 单次 Present 原子提交：
        // 视频帧 + UI overlay 在同一 vsync 周期，无 plane 合成额外延迟）。
        ID3D11RenderTargetView* null_rtvs[] = { nullptr };
        ctx->OMSetRenderTargets(1, null_rtvs, nullptr);

        if (ui) ui->render();

        // Present 前等 frame-latency waitable —— 配合 swap_chain 的 SetMaximumFrameLatency(1)，
        // 把"管线占满到 vsync"的额外帧时延吃掉（DXGI 推荐路径）。
        const bool tearing =
            (swap_chain->active_present_mode() == MC_PRESENT_MODE_TEARING);
        // Phase 4 frame doubling:动态计算 sync_interval 让 GPU 严格锁 N 个 vsync 周期。
        //   30fps source / 60Hz display → SyncInterval=2 = 33.33ms/frame 严格匹配 source;
        //   60fps source / 60Hz display → SyncInterval=1;
        //   嗅探未稳定(头 < 8 帧)用 SyncInterval=0 立即 present(allow_tearing 兜底)。
        // 与 wait_until/PTS-paced 互斥:GPU 锁帧主导节奏,不再应用层 sleep。
        const int64_t vsync_period = frame_period_ns.load(std::memory_order_acquire);
        const uint32_t sync_interval = sync_est.compute_sync_interval(vsync_period);
        const int64_t now_ns = pal::Clock::now_ns();
        if (swap_chain->present(sync_interval, tearing && sync_interval == 0) != MC_OK) {
            return;
        }
        presents.fetch_add(1, std::memory_order_relaxed);
        if (epoch) epoch->on_presented(now_ns);

        // 端到端延时统计（P0-4）：frame.arrival_qpc_ns 来自 RTP 第一包的 QPC 戳，
        // 经 depack→controller→codec 全链透传；为 0 表示该帧无法溯源（如 codec
        // MFT 黑盒未透传 attribute 且 PTS ring 兜底未命中）。
        if (frame.arrival_qpc_ns > 0) {
            const int64_t e2e_ns = now_ns - frame.arrival_qpc_ns;
            // 防御异常 0 / clock skew 越界（>2 s 几乎必然是脏数据）。
            if (e2e_ns > 0 && e2e_ns < 2'000'000'000LL) {
                latency_probe.record(e2e_ns);
            }
        } else {
            latency_probe.samples_zero.fetch_add(1, std::memory_order_relaxed);
        }
        latency_probe.samples_total.fetch_add(1, std::memory_order_relaxed);
        latency_probe.log_if_due(now_ns);
    }
};

RenderD3d11::RenderD3d11(Config cfg) : impl_{std::make_unique<Impl>()} {
    impl_->cfg = std::move(cfg);
}

RenderD3d11::~RenderD3d11() { stop(); }

mc_status_t RenderD3d11::start() noexcept {
    if (!impl_->cfg.device || !impl_->cfg.hwnd) return MC_ERR_INVALID_ARG;

    impl_->cfg.device->GetImmediateContext(&impl_->ctx);

    SwapChainCreateInfo sc;
    sc.device       = impl_->cfg.device;
    sc.hwnd         = impl_->cfg.hwnd;
    sc.profile_hint = impl_->cfg.profile_hint;
    impl_->swap_chain = std::make_unique<SwapChain>(sc);
    if (mc_status_t s = impl_->swap_chain->create(); s != MC_OK) return s;

    if (FAILED(impl_->create_pipeline_state())) return MC_ERR_INTERNAL;
    impl_->pipeline_ready = true;

    impl_->frame_period_ns.store(query_frame_period_ns(impl_->cfg.hwnd),
                                  std::memory_order_release);

    impl_->dcomp = std::make_unique<DcompRoot>();
    impl_->dcomp->create(impl_->cfg.hwnd);

    // 性能量度规范 §7.1 渲染管线 label / gauge 初值。
    //   profile_target = 调用方传入的 hint(自适应前)
    //   profile_actual = SwapChain auto-resolve 后的实际档(无 PresentMon 时退化为 active_profile)
    //   dcomp_on / frame_latency_waitable_on = 子系统启用旗标(SwapChain 默认带 waitable)
    auto& reg = pal::metric::Registry::instance();
    reg.gauge("mc.render.profile_target")
        .set(static_cast<int64_t>(impl_->cfg.profile_hint));
    reg.gauge("mc.render.profile_actual")
        .set(static_cast<int64_t>(impl_->swap_chain->active_profile()));
    reg.gauge("mc.render.dcomp_on").set(impl_->dcomp ? 1 : 0);
    reg.gauge("mc.render.frame_latency_waitable_on").set(1);
    reg.gauge("mc.render.allow_tearing_on").set(
        impl_->swap_chain->active_present_mode() == MC_PRESENT_MODE_TEARING ? 1 : 0);
    {
        const int64_t period_ns = impl_->frame_period_ns.load(std::memory_order_acquire);
        if (period_ns > 0) {
            reg.gauge("mc.render.display_refresh_hz")
                .set(1'000'000'000LL / period_ns);
        }
    }

    // UI overlay：D2D + DirectWrite 在同一 D3D11 device 上共享 backbuffer。
    // 失败不致命——视频仍可放（仅缺 UI），打 warn 继续。
    {
        UiOverlay::Config uc;
        uc.device = impl_->cfg.device;
        impl_->ui = std::make_unique<UiOverlay>(std::move(uc));
        if (FAILED(impl_->ui->initialize())) {
            MCP_LOGF(pal::LogLevel::warn,
                     "RenderD3d11: UiOverlay init failed; continuing without UI");
            impl_->ui.reset();
        }
    }

    // PresentEpoch 的 redraw / commit 回调都在 T5 上执行（force_redraw 由 T5 watchdog 触发）。
    // last_good / has_last_good 仅 T5 写入，回调内可直接访问。
    auto redraw = [this] {
        if (impl_->has_last_good) impl_->render_locked(impl_->last_good);
        else                       impl_->render_ui_only_locked();
    };
    auto commit = [this] {
        if (impl_->dcomp) (void)impl_->dcomp->commit();
    };
    impl_->epoch = std::make_unique<PresentEpoch>(redraw, commit);

    // 启动 T5 渲染线程：开机第一帧由 T5 自己画（让 connecting stage 立即可见）。
    impl_->wake_event.reset(::CreateEventW(nullptr, FALSE /*auto-reset*/, FALSE, nullptr));
    if (!impl_->wake_event.valid()) {
        MCP_LOG_ERROR("RenderD3d11: CreateEvent for T5 wake failed");
        return MC_ERR_INTERNAL;
    }
    impl_->render_stop.store(false, std::memory_order_release);
    impl_->render_thread = std::thread([impl = impl_.get()] { impl->render_thread_loop(); });
    return MC_OK;
}

void RenderD3d11::stop() noexcept {
    if (!impl_) return;
    // 阶段 1：通知 T5 退出并等其结束。在 T5 结束前不能销毁 swap_chain / epoch / dcomp。
    impl_->render_stop.store(true, std::memory_order_release);
    impl_->video_clock.request_stop();         // 唤醒可能阻塞在 wait_until 的 T5
    if (impl_->wake_event.valid()) ::SetEvent(impl_->wake_event.get());
    if (impl_->render_thread.joinable()) impl_->render_thread.join();
    impl_->wake_event.reset();

    // 阶段 2：T5 已退出，single-thread cleanup，无需锁。
    impl_->epoch.reset();
    if (impl_->dcomp) impl_->dcomp->destroy();
    impl_->dcomp.reset();
    if (impl_->ui) impl_->ui->unbind_backbuffer();
    impl_->ui.reset();
    impl_->invalidate_srv_cache();
    impl_->rtv.Reset();
    impl_->vs.Reset();
    impl_->ps.Reset();
    impl_->samp.Reset();
    impl_->raster.Reset();
    impl_->ctx.Reset();
    impl_->swap_chain.reset();
    impl_->has_last_good = false;
    impl_->last_good = VideoFrame{};
    impl_->video_clock.reset();                // 清 anchor + coeff;stop 状态保持
    {
        std::scoped_lock lk{impl_->pending_mu};
        impl_->pending_queue.clear();
    }
}

void RenderD3d11::on_admitted(const VideoFrame& frame) noexcept {
    // 入 pending_queue + signal T5。不直接渲染:渲染由 T5 完成。
    // codec reorder buffer drain 时会 burst emit 多帧;FIFO queue 保留全部直到 T5 消费。
    // 队列满时 drop oldest(最旧的 PTS),以跟上 codec 节奏 — VLC vout 同等策略(es_out 满
    // 时丢最老 block 让最新内容能上屏)。
    if (!impl_->wake_event.valid()) return;
    {
        std::scoped_lock lk{impl_->pending_mu};
        if (impl_->pending_queue.size() >= Impl::kPendingQueueCap) {
            impl_->pending_queue.pop_front();
            impl_->pending_drops.fetch_add(1, std::memory_order_relaxed);
        }
        impl_->pending_queue.push_back(frame);
    }
    ::SetEvent(impl_->wake_event.get());
}

void RenderD3d11::tick_watchdog() noexcept {
    // T5 自驱 watchdog；保留接口仅为向后兼容。
}

mc_render_profile_t RenderD3d11::active_profile() const noexcept {
    return impl_->swap_chain ? impl_->swap_chain->active_profile() : MC_RENDER_PROFILE_AUTO;
}

mc_present_mode_t RenderD3d11::active_present_mode() const noexcept {
    return impl_->swap_chain ? impl_->swap_chain->active_present_mode() : MC_PRESENT_MODE_UNKNOWN;
}

uint64_t RenderD3d11::present_count() const noexcept {
    return impl_->presents.load(std::memory_order_relaxed);
}

uint64_t RenderD3d11::skip_count() const noexcept {
    return impl_->epoch ? impl_->epoch->skip_count() : 0;
}

void RenderD3d11::tick_ui() noexcept {
    // 仅推进 UI overlay 时间基；渲染由 T5 自驱（每 ~frame_period 唤醒一次）。
    // 不再 trigger render —— 避免与 T5 同时拿 immediate ctx。
    if (!impl_->ui) return;
    impl_->ui->tick(pal::Clock::now_ns());
}

UiOverlay* RenderD3d11::ui_overlay() noexcept {
    return impl_->ui.get();
}

// ─── T5 主循环 ──────────────────────────────────────────────────
void RenderD3d11::Impl::render_thread_loop() noexcept {
    pal::ThreadRegistration reg;
    pal::ThreadOptions opt;
    opt.name        = "mc-player T5 Render";
    opt.mmcss_task  = pal::MmcssTask::playback;
    opt.affinity    = pal::ThreadAffinityHint::pcore_only;
    reg.apply(opt);

    // 开机首帧：让 UI（CEmpty / connecting）立即可见。
    if (epoch) (void)epoch->begin_epoch();
    render_ui_only_locked();

    while (!render_stop.load(std::memory_order_acquire)) {
        // 唤醒周期：~frame_period（60Hz ≈ 16ms）。够频以驱动 UI 动画 +
        // 探测陈旧区域；过短会浪费 CPU。无显示器信息时 fallback 16ms。
        const int64_t period_ns = frame_period_ns.load(std::memory_order_acquire);
        const DWORD timeout_ms = period_ns > 0
            ? static_cast<DWORD>(std::clamp<int64_t>(period_ns / 1'000'000, 1, 50))
            : 16;
        const DWORD wr = ::WaitForSingleObject(wake_event.get(), timeout_ms);
        (void)wr;
        if (render_stop.load(std::memory_order_acquire)) break;

        // 从 FIFO queue 取一帧渲染。codec reorder buffer 已保证入队 PTS 单调,T5 按
        // FIFO 顺序消费即等于 display 顺序。每次 wake 仅出 1 帧:配合 wait_for_frame_latency
        // 节奏在 vsync 边界 Present,避免 burst-render 多帧瞬间堆到一个 vsync 内。
        // queue 还有剩余时不复位 wake_event(auto-reset),下次 SetEvent 会再唤醒;若
        // queue 当前次没消费完(后续多帧累积),T5 在下一 timeout 周期(~16ms)继续 pop。
        VideoFrame frame;
        bool got_frame = false;
        std::size_t remaining_after_pop = 0;
        {
            std::scoped_lock lk{pending_mu};
            if (!pending_queue.empty()) {
                frame = std::move(pending_queue.front());
                pending_queue.pop_front();
                remaining_after_pop = pending_queue.size();
                got_frame = true;
            }
        }
        // queue 还有剩余 → 立即重置 wake_event 让下个 wait 立刻返回
        // (auto-reset event:WaitForSingleObject 已消费 signal,需手动 set 让下次 wait 不阻塞 timeout)
        if (got_frame && remaining_after_pop > 0) {
            ::SetEvent(wake_event.get());
        }

        if (got_frame) {
            // Phase 4 主路径 — vsync interval=2 frame doubling(GPU 锁帧):
            //   1. sync_est.observe(pts) 嗅探 source frame_period(EMA last 16 帧);
            //   2. master_update(arrival_qpc, pts) 维护 video_clock 状态供后续 audio
            //      master 接入用,Phase 4 本身不读 video_clock(GPU 锁帧主导节奏);
            //   3. render_locked(frame) 内部 swap_chain.present(sync_interval=2),
            //      Present 内部 block 直到 N vsync,T5 处理速率严格 = display/N。
            //   30fps source / 60Hz display → SyncInterval=2 = 33.33ms/frame 严格;
            //   消除"短-短-长" 因 vsync drift 引起的跳动。
            const int64_t now_qpc     = pal::Clock::now_ns();
            const int64_t arrival_qpc = (frame.arrival_qpc_ns > 0)
                                            ? frame.arrival_qpc_ns : now_qpc;
            sync_est.observe(frame.pts_us);
            video_clock.master_update(arrival_qpc, frame.pts_us, /*rate=*/1.0);

            const uint64_t pcnt = presents.load(std::memory_order_relaxed);
            if (pcnt < 6 || (pcnt + 1) % 60 == 0) {
                const int64_t vsync_period = frame_period_ns.load(std::memory_order_acquire);
                const uint32_t si = sync_est.compute_sync_interval(vsync_period);
                MCP_LOGF(pal::LogLevel::info,
                    "T5[stat] #%llu pts=%lld src_period=%lld vsync_period=%lld "
                    "sync_interval=%u queue_after=%zu",
                    static_cast<unsigned long long>(pcnt),
                    static_cast<long long>(frame.pts_us),
                    static_cast<long long>(sync_est.source_period_avg_ns.load()),
                    static_cast<long long>(vsync_period),
                    si, remaining_after_pop);
            }

            (void)epoch->begin_epoch();
            // last_good anchor-only 更新:仅 IDR 帧覆盖 last_good(防 partial-decode 污染
            // watchdog redraw)。
            if (frame.is_keyframe || !has_last_good) {
                last_good     = frame;
                has_last_good = true;
                render_locked(last_good);
            } else {
                render_locked(frame);
            }
            continue;
        }

        // 无新帧：playing 态走 watchdog（PresentEpoch::tick 内部决定是否 force_redraw）；
        // empty/connecting/error 态则每 wake 周期 redraw_ui_only 让动画相位前进。
        if (has_last_good) {
            int64_t period = frame_period_ns.load(std::memory_order_acquire);
            if (period <= 0) {
                period = query_frame_period_ns(cfg.hwnd);
                if (period > 0) frame_period_ns.store(period, std::memory_order_release);
            }
            if (period > 0 && epoch) {
                epoch->tick(pal::Clock::now_ns(), period, kWatchdogPeriodsThreshold);
            }
        } else {
            if (epoch) (void)epoch->begin_epoch();
            render_ui_only_locked();
        }
    }
}

}  // namespace mcp::media
