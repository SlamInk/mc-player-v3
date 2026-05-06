#include "media/render_swap_chain.h"

#include <dxgi1_3.h>
#include <dxgi1_5.h>

#include "pal/error.h"
#include "pal/log.h"
#include "pal/metric.h"

#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace mcp::media {

namespace {

constexpr UINT kBufferCount = 2;

mc_render_profile_t resolve_profile(mc_render_profile_t hint, bool tearing) noexcept {
    if (hint != MC_RENDER_PROFILE_AUTO) return hint;
    return tearing ? MC_RENDER_PROFILE_BALANCED : MC_RENDER_PROFILE_COMPAT;
}

mc_present_mode_t profile_present_mode(mc_render_profile_t p) noexcept {
    switch (p) {
        case MC_RENDER_PROFILE_COMPAT:           return MC_PRESENT_MODE_COMPOSED_FLIP;
        case MC_RENDER_PROFILE_BALANCED:         return MC_PRESENT_MODE_HARDWARE_INDEPENDENT_FLIP;
        case MC_RENDER_PROFILE_EXTREME:          return MC_PRESENT_MODE_TEARING;
        case MC_RENDER_PROFILE_ULTIMATE_DCOMP:   return MC_PRESENT_MODE_HW_COMPOSED_INDEPENDENT_FLIP;
        default:                                 return MC_PRESENT_MODE_UNKNOWN;
    }
}

}  // namespace

SwapChain::SwapChain(SwapChainCreateInfo info) : info_{std::move(info)} {}
SwapChain::~SwapChain() {
    if (waitable_) ::CloseHandle(waitable_);
}

mc_status_t SwapChain::create() noexcept {
    if (!info_.device || !info_.hwnd) return MC_ERR_INVALID_ARG;

    HRESULT hr = ::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_));
    if (FAILED(hr)) return pal::status_from_hresult(hr);

    BOOL tearing_supported = FALSE;
    factory_->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                   &tearing_supported, sizeof(tearing_supported));

    active_profile_      = resolve_profile(info_.profile_hint, tearing_supported == TRUE);
    active_present_mode_ = profile_present_mode(active_profile_);

    if (info_.width == 0 || info_.height == 0) {
        RECT rc{};
        if (::GetClientRect(info_.hwnd, &rc)) {
            info_.width  = static_cast<UINT>(rc.right  - rc.left);
            info_.height = static_cast<UINT>(rc.bottom - rc.top);
        }
        if (info_.width == 0)  info_.width  = 1;
        if (info_.height == 0) info_.height = 1;
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width            = info_.width;
    desc.Height           = info_.height;
    desc.Format           = info_.format;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount      = kBufferCount;
    desc.Scaling          = DXGI_SCALING_STRETCH;
    desc.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode        = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags            = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (tearing_supported) desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    ComPtr<IDXGISwapChain1> sc1;
    hr = factory_->CreateSwapChainForHwnd(info_.device.Get(), info_.hwnd,
                                           &desc, nullptr, nullptr, &sc1);
    if (FAILED(hr)) {
        MCP_LOGF(pal::LogLevel::error,
                 "SwapChain: CreateSwapChainForHwnd failed hr=0x%08lX", hr);
        return pal::status_from_hresult(hr);
    }
    factory_->MakeWindowAssociation(info_.hwnd, DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&swap_chain_);

    ComPtr<IDXGISwapChain2> sc2;
    if (SUCCEEDED(swap_chain_.As(&sc2))) {
        sc2->SetMaximumFrameLatency(1);
        waitable_ = sc2->GetFrameLatencyWaitableObject();
    }
    MCP_LOGF(pal::LogLevel::info,
             "SwapChain: %ux%u tearing=%d profile=%d",
             desc.Width, desc.Height, tearing_supported, static_cast<int>(active_profile_));
    return MC_OK;
}

mc_status_t SwapChain::resize(uint32_t width, uint32_t height,
                               ClearViewsFn clear_cb, void* clear_user) noexcept {
    if (!swap_chain_) return MC_ERR_INVALID_STATE;
    // 性能量度规范 §7.4 mc.render.resize_buffers_count。
    pal::metric::Registry::instance().counter("mc.render.resize_buffers_count").inc();
    if (clear_cb) clear_cb(clear_user);

    ComPtr<ID3D11DeviceContext> ctx;
    info_.device->GetImmediateContext(&ctx);
    ctx->ClearState();
    ctx->Flush();

    DXGI_SWAP_CHAIN_DESC1 desc{};
    swap_chain_->GetDesc1(&desc);
    HRESULT hr = swap_chain_->ResizeBuffers(kBufferCount, width, height,
                                             desc.Format, desc.Flags);
    if (FAILED(hr)) {
        // ADD §5.10.3：ResizeBuffers 前未 ClearState + 未释放 back buffer 引用的 SRV/RTV/UAV
        // 即返 DXGI_ERROR_INVALID_CALL。本路径已先 ClearState + clear_cb,正常应不命中此分支;
        // 命中即架构不变量违反 → mc.render.resize_clearstate_violation_count（性能量度规范
        // §7.4 / §11.1 必达指标 = 0 永久）。
        if (hr == DXGI_ERROR_INVALID_CALL) {
            pal::metric::Registry::instance()
                .counter("mc.render.resize_clearstate_violation_count").inc();
        }
        MCP_LOGF(pal::LogLevel::warn,
                 "SwapChain: ResizeBuffers failed hr=0x%08lX", hr);
        return pal::status_from_hresult(hr);
    }
    info_.width  = width;
    info_.height = height;
    return MC_OK;
}

bool SwapChain::wait_for_frame_latency(uint32_t timeout_ms) noexcept {
    if (!waitable_) return true;
    return ::WaitForSingleObjectEx(waitable_, timeout_ms, TRUE) == WAIT_OBJECT_0;
}

mc_status_t SwapChain::present(bool allow_tearing) noexcept {
    if (!swap_chain_) return MC_ERR_INVALID_STATE;
    UINT flags = 0;
    if (allow_tearing) flags |= DXGI_PRESENT_ALLOW_TEARING;
    DXGI_PRESENT_PARAMETERS pp{};
    HRESULT hr = swap_chain_->Present1(allow_tearing ? 0 : 1, flags, &pp);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        return MC_ERR_DEVICE_LOST;
    }
    if (FAILED(hr)) return pal::status_from_hresult(hr);
    return MC_OK;
}

ComPtr<ID3D11Texture2D> SwapChain::back_buffer() const noexcept {
    ComPtr<ID3D11Texture2D> bb;
    if (swap_chain_) {
        swap_chain_->GetBuffer(0, IID_PPV_ARGS(&bb));
    }
    return bb;
}

}  // namespace mcp::media
