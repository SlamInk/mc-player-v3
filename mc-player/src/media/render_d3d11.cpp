#include "media/render_d3d11.h"

#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <atomic>
#include <mutex>

#include "media/ui_overlay.h"
#include "pal/clock.h"
#include "pal/error.h"
#include "pal/log.h"

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

    std::mutex                              mu;        // 跨 decoder→render 线程保护
    VideoFrame                              last_good;
    bool                                    has_last_good = false;
    std::atomic<int64_t>                    frame_period_ns{0};   // 0 = 未知（watchdog 走 fallback）

    ComPtr<ID3D11DeviceContext>             ctx;
    ComPtr<ID3D11RenderTargetView>          rtv;
    ComPtr<ID3D11VertexShader>              vs;
    ComPtr<ID3D11PixelShader>               ps;
    ComPtr<ID3D11SamplerState>              samp;
    ComPtr<ID3D11RasterizerState>           raster;

    bool                                    pipeline_ready = false;
    std::atomic<uint64_t>                   presents{0};

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
    }

    static void clear_views_thunk(void* user) noexcept {
        if (auto* self = static_cast<Impl*>(user)) self->clear_views();
    }

    // 仅画 UI（无视频帧）；start() 后开机第一帧 / 长时间 freeze 期可调用，让用户立即看到 UI。
    // 不持有外部锁，调用方自行同步（当前两处调用：start() 末尾、render_locked 中视频缺失时）。
    void render_ui_only_locked() noexcept {
        if (!swap_chain || !pipeline_ready) return;
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
        if (swap_chain->present(tearing) == MC_OK) {
            presents.fetch_add(1, std::memory_order_relaxed);
        }
        if (epoch) epoch->on_presented(pal::Clock::now_ns());
    }

    void render_locked(const VideoFrame& frame) noexcept {
        if (!swap_chain || !pipeline_ready) return;

        if (FAILED(ensure_rtv())) return;
        if (!frame.dxva_texture) return;

        // 创建 array slice 上的 NV12 双 SRV（Y=R8，UV=R8G8）。
        D3D11_SHADER_RESOURCE_VIEW_DESC y_desc{};
        y_desc.Format                          = DXGI_FORMAT_R8_UNORM;
        y_desc.ViewDimension                   = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        y_desc.Texture2DArray.MostDetailedMip  = 0;
        y_desc.Texture2DArray.MipLevels        = 1;
        y_desc.Texture2DArray.FirstArraySlice  = frame.dxva_array_slice;
        y_desc.Texture2DArray.ArraySize        = 1;
        ComPtr<ID3D11ShaderResourceView> y_srv;
        if (FAILED(cfg.device->CreateShaderResourceView(frame.dxva_texture.Get(),
                                                          &y_desc, &y_srv))) return;

        D3D11_SHADER_RESOURCE_VIEW_DESC uv_desc = y_desc;
        uv_desc.Format = DXGI_FORMAT_R8G8_UNORM;
        ComPtr<ID3D11ShaderResourceView> uv_srv;
        if (FAILED(cfg.device->CreateShaderResourceView(frame.dxva_texture.Get(),
                                                         &uv_desc, &uv_srv))) return;

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

        const bool tearing =
            (swap_chain->active_present_mode() == MC_PRESENT_MODE_TEARING);
        if (swap_chain->present(tearing) == MC_OK) {
            presents.fetch_add(1, std::memory_order_relaxed);
        }
        if (epoch) epoch->on_presented(pal::Clock::now_ns());
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

    auto redraw = [this] {
        std::scoped_lock lk{impl_->mu};
        if (impl_->has_last_good) impl_->render_locked(impl_->last_good);
        else                       impl_->render_ui_only_locked();
    };
    auto commit = [this] {
        if (impl_->dcomp) (void)impl_->dcomp->commit();
    };
    impl_->epoch = std::make_unique<PresentEpoch>(redraw, commit);

    // 开机第一帧：让 UI（CEmpty）立即可见，无需等视频。
    {
        std::scoped_lock lk{impl_->mu};
        impl_->render_ui_only_locked();
    }
    return MC_OK;
}

void RenderD3d11::stop() noexcept {
    if (!impl_) return;
    std::scoped_lock lk{impl_->mu};
    impl_->epoch.reset();
    if (impl_->dcomp) impl_->dcomp->destroy();
    impl_->dcomp.reset();
    if (impl_->ui) impl_->ui->unbind_backbuffer();
    impl_->ui.reset();
    impl_->rtv.Reset();
    impl_->vs.Reset();
    impl_->ps.Reset();
    impl_->samp.Reset();
    impl_->raster.Reset();
    impl_->ctx.Reset();
    impl_->swap_chain.reset();
    impl_->has_last_good = false;
    impl_->last_good = VideoFrame{};
}

void RenderD3d11::on_admitted(const VideoFrame& frame) noexcept {
    std::scoped_lock lk{impl_->mu};
    if (!impl_->swap_chain || !impl_->epoch) return;
    (void)impl_->epoch->begin_epoch();
    impl_->last_good     = frame;
    impl_->has_last_good = true;
    impl_->render_locked(frame);
}

void RenderD3d11::tick_watchdog() noexcept {
    if (!impl_->epoch) return;
    int64_t period = impl_->frame_period_ns.load(std::memory_order_acquire);
    if (period <= 0) {
        // 显示器探测失败：刷新一次，仍失败时跳过本次 tick（保持 0 让 PresentEpoch::tick 退化为 noop）。
        period = query_frame_period_ns(impl_->cfg.hwnd);
        if (period > 0) impl_->frame_period_ns.store(period, std::memory_order_release);
        if (period <= 0) return;
    }
    impl_->epoch->tick(pal::Clock::now_ns(), period, kWatchdogPeriodsThreshold);
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
    if (!impl_->ui) return;
    impl_->ui->tick(pal::Clock::now_ns());

    std::scoped_lock lk{impl_->mu};
    // playing 态由视频帧自然驱动；empty/connecting/error 态需要 tick 触发 Present 才能跑动画。
    if (!impl_->has_last_good) {
        impl_->render_ui_only_locked();
    }
}

UiOverlay* RenderD3d11::ui_overlay() noexcept {
    return impl_->ui.get();
}

}  // namespace mcp::media
