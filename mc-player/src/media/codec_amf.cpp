#include "media/codec_amf.h"

#include <dxgi.h>

#include "media/codec_vendor_base.h"
#include "pal/error.h"
#include "pal/log.h"

using Microsoft::WRL::ComPtr;

namespace mcp::media {

namespace {

// AMF 关键入口 — 对位 AMF Runtime 公共接口(amfrt64.dll)。
// 仅 AMFInit 是 C 入口,Factory / Context / Component 都通过 AMF::AMFFactory*
// 接口取得,无独立 GetProcAddress。AMFQueryVersion 可探测 SDK 版本。
constexpr const char* kEntryPoints[] = {
    "AMFInit",
    "AMFQueryVersion",
};

uint32_t query_vendor_id(ID3D11Device* device) noexcept {
    if (!device) return 0;
    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) return 0;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_device->GetAdapter(&adapter))) return 0;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc))) return 0;
    return desc.VendorId;
}

}  // namespace

struct CodecAmf::Impl {
    Config                  cfg;
    HMODULE                 amf_handle  = nullptr;
    CodecAmf::StartReason   last_reason = CodecAmf::StartReason::ok;

    bool load_sdk() noexcept {
        amf_handle = ::LoadLibraryW(L"amfrt64.dll");
        return amf_handle != nullptr;
    }

    bool probe_entry_points() const noexcept {
        if (!amf_handle) return false;
        for (const char* name : kEntryPoints) {
            if (::GetProcAddress(amf_handle, name) == nullptr) {
                MCP_LOGF(pal::LogLevel::warn,
                         "CodecAmf: missing entry point '%s' in amfrt64.dll", name);
                return false;
            }
        }
        return true;
    }

    void unload_sdk() noexcept {
        if (amf_handle) {
            ::FreeLibrary(amf_handle);
            amf_handle = nullptr;
        }
    }
};

CodecAmf::CodecAmf(Config cfg) : impl_{std::make_unique<Impl>()} {
    impl_->cfg = std::move(cfg);
}

CodecAmf::~CodecAmf() { stop(); }

mc_status_t CodecAmf::start() noexcept {
    if (!impl_->cfg.device) {
        impl_->last_reason = StartReason::device_invalid;
        return MC_ERR_INVALID_ARG;
    }

    const uint32_t vid = query_vendor_id(impl_->cfg.device.Get());
    if (vid != kVendorIdAmd) {
        impl_->last_reason = StartReason::vendor_mismatch;
        MCP_LOGF(pal::LogLevel::info,
                 "CodecAmf: vendor mismatch (adapter VendorId=0x%04X, expected 0x1002)",
                 vid);
        return MC_ERR_UNSUPPORTED;
    }

    if (!impl_->load_sdk()) {
        impl_->last_reason = StartReason::sdk_missing;
        MCP_LOG_INFO("CodecAmf: amfrt64.dll not present (sdk_missing)");
        return MC_ERR_NO_HARDWARE;
    }

    if (!impl_->probe_entry_points()) {
        impl_->unload_sdk();
        impl_->last_reason = StartReason::sdk_init_failed;
        MCP_LOG_WARN("CodecAmf: SDK loaded but required entry points missing");
        return MC_ERR_INTERNAL;
    }

    // Phase 7b: AMFInit → AMFFactory::CreateContext → InitDX11(d3d11) →
    //   CreateComponent(AMFVideoDecoderUVD_H264_AVC / _H265_HEVC) →
    //   设 AMF_VIDEO_DECODER_REORDER_MODE = AMF_VIDEO_DECODER_MODE_LOW_LATENCY →
    //   Submit/QueryOutput 异步管线 → AMFSurface::GetPlane(PACKED)::GetNative()
    //   即 D3D11 NV12 texture。当前结构性骨架:卸载 SDK 让 controller 降到档 2。
    impl_->unload_sdk();
    impl_->last_reason = StartReason::sdk_decode_pending;
    MCP_LOG_INFO("CodecAmf: SDK loaded, decode pipeline pending Phase 7b — fall back to lower tier");
    return MC_ERR_UNSUPPORTED;
}

void CodecAmf::submit(std::vector<uint8_t> /*au_bytes*/, int64_t /*pts_us*/,
                       int64_t /*arrival_qpc_ns*/) noexcept {
    // Phase 7b: AMFComponent::Submit(amf_buffer)。
}

void CodecAmf::flush() noexcept {
    // Phase 7b: AMFComponent::Drain + 排空 QueryOutput。
}

void CodecAmf::stop() noexcept {
    if (!impl_) return;
    // Phase 7b: AMFComponent::Terminate + AMFContext::Terminate。
    impl_->unload_sdk();
}

CodecAmf::StartReason CodecAmf::last_start_reason() const noexcept {
    return impl_ ? impl_->last_reason : StartReason::device_invalid;
}

bool CodecAmf::probe_sdk_dll_present() noexcept {
    HMODULE h = ::LoadLibraryW(L"amfrt64.dll");
    if (!h) return false;
    ::FreeLibrary(h);
    return true;
}

const char* CodecAmf::reason_label(StartReason r) noexcept {
    switch (r) {
        case StartReason::ok:                  return "ok";
        case StartReason::vendor_mismatch:     return "vendor_mismatch";
        case StartReason::sdk_missing:         return "sdk_missing";
        case StartReason::sdk_init_failed:     return "sdk_init_failed";
        case StartReason::sdk_decode_pending:  return "sdk_decode_pending";
        case StartReason::profile_unsupported: return "profile_unsupported";
        case StartReason::device_invalid:      return "device_invalid";
    }
    return "unknown";
}

}  // namespace mcp::media
