#include "media/codec_onevpl.h"

#include <dxgi.h>

#include "media/codec_vendor_base.h"
#include "pal/error.h"
#include "pal/log.h"

using Microsoft::WRL::ComPtr;

namespace mcp::media {

namespace {

// oneVPL 关键入口 — 对位 oneVPL 2.x 公共接口（vpl.dll；旧 Media SDK 1.x 在 libmfx.dll）。
// vpl.dll 与 libmfx.dll 共享前缀 MFX*；2.x dispatcher 由 MFXLoad 路由。
//
// 仅记录"能否全部 GetProcAddress"作为 sdk_init_failed 判据;实际 mfx* ABI 与
// session 配置留 Phase 6b。
constexpr const char* kEntryPointsVpl[] = {
    "MFXLoad",                       // oneVPL 2.x dispatcher 入口
    "MFXCreateSession",
    "MFXClose",
    "MFXVideoDECODE_Init",
    "MFXVideoDECODE_DecodeHeader",
    "MFXVideoDECODE_DecodeFrameAsync",
    "MFXVideoDECODE_Close",
    "MFXVideoDECODE_QueryIOSurf",
    "MFXVideoCORE_SetHandle",
    "MFXVideoCORE_SyncOperation",
    "MFXUnload",
};

// libmfx.dll(Media SDK 1.x)入口集合略简：无 MFXLoad/MFXUnload,直接 MFXInit。
constexpr const char* kEntryPointsLibmfx[] = {
    "MFXInit",
    "MFXClose",
    "MFXVideoDECODE_Init",
    "MFXVideoDECODE_DecodeHeader",
    "MFXVideoDECODE_DecodeFrameAsync",
    "MFXVideoDECODE_Close",
    "MFXVideoDECODE_QueryIOSurf",
    "MFXVideoCORE_SetHandle",
    "MFXVideoCORE_SyncOperation",
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

struct CodecOneVPL::Impl {
    Config                       cfg;
    HMODULE                      sdk_handle = nullptr;
    bool                         is_vpl     = false;     // true=vpl.dll / false=libmfx.dll
    CodecOneVPL::StartReason     last_reason = CodecOneVPL::StartReason::ok;

    bool load_sdk() noexcept {
        sdk_handle = ::LoadLibraryW(L"vpl.dll");
        if (sdk_handle) { is_vpl = true; return true; }
        sdk_handle = ::LoadLibraryW(L"libmfx.dll");
        if (sdk_handle) { is_vpl = false; return true; }
        return false;
    }

    bool probe_entry_points() const noexcept {
        if (!sdk_handle) return false;
        const char* const* table = is_vpl ? kEntryPointsVpl : kEntryPointsLibmfx;
        const std::size_t  count = is_vpl ? std::size(kEntryPointsVpl)
                                            : std::size(kEntryPointsLibmfx);
        for (std::size_t i = 0; i < count; ++i) {
            if (::GetProcAddress(sdk_handle, table[i]) == nullptr) {
                MCP_LOGF(pal::LogLevel::warn,
                         "CodecOneVPL: missing entry point '%s' in %s",
                         table[i], is_vpl ? "vpl.dll" : "libmfx.dll");
                return false;
            }
        }
        return true;
    }

    void unload_sdk() noexcept {
        if (sdk_handle) {
            ::FreeLibrary(sdk_handle);
            sdk_handle = nullptr;
        }
    }
};

CodecOneVPL::CodecOneVPL(Config cfg) : impl_{std::make_unique<Impl>()} {
    impl_->cfg = std::move(cfg);
}

CodecOneVPL::~CodecOneVPL() { stop(); }

mc_status_t CodecOneVPL::start() noexcept {
    if (!impl_->cfg.device) {
        impl_->last_reason = StartReason::device_invalid;
        return MC_ERR_INVALID_ARG;
    }

    const uint32_t vid = query_vendor_id(impl_->cfg.device.Get());
    if (vid != kVendorIdIntel) {
        impl_->last_reason = StartReason::vendor_mismatch;
        MCP_LOGF(pal::LogLevel::info,
                 "CodecOneVPL: vendor mismatch (adapter VendorId=0x%04X, expected 0x8086)",
                 vid);
        return MC_ERR_UNSUPPORTED;
    }

    if (!impl_->load_sdk()) {
        impl_->last_reason = StartReason::sdk_missing;
        MCP_LOG_INFO("CodecOneVPL: vpl.dll / libmfx.dll not present (sdk_missing)");
        return MC_ERR_NO_HARDWARE;
    }

    MCP_LOGF(pal::LogLevel::info,
             "CodecOneVPL: SDK loaded via %s",
             impl_->is_vpl ? "vpl.dll (oneVPL 2.x)" : "libmfx.dll (Media SDK 1.x)");

    if (!impl_->probe_entry_points()) {
        impl_->unload_sdk();
        impl_->last_reason = StartReason::sdk_init_failed;
        MCP_LOG_WARN("CodecOneVPL: SDK loaded but required entry points missing");
        return MC_ERR_INTERNAL;
    }

    // Phase 6b: MFXLoad → MFXCreateSession(MFX_IMPL_HARDWARE) → MFXVideoCORE_SetHandle
    //   (D3D11 device 共享) → MFXVideoDECODE_DecodeHeader → mfxVideoParam
    //   (AsyncDepth=1, IOPattern=MFX_IOPATTERN_OUT_VIDEO_MEMORY) → DecodeInit →
    //   主循环 mfxBitstream(DataFlag|=MFX_BITSTREAM_COMPLETE_FRAME) → DecodeFrameAsync
    //   + SyncOperation → mfxFrameSurface1::Data.MemId 即 D3D11 NV12 texture(零拷贝)。
    // 当前结构性骨架:卸载 SDK 让 controller 降到档 2/3。
    impl_->unload_sdk();
    impl_->last_reason = StartReason::sdk_decode_pending;
    MCP_LOG_INFO("CodecOneVPL: SDK loaded, decode pipeline pending Phase 6b — fall back to lower tier");
    return MC_ERR_UNSUPPORTED;
}

void CodecOneVPL::submit(std::vector<uint8_t> /*au_bytes*/, int64_t /*pts_us*/,
                          int64_t /*arrival_qpc_ns*/) noexcept {
    // Phase 6b: mfxBitstream 累积 + MFXVideoDECODE_DecodeFrameAsync。
}

void CodecOneVPL::flush() noexcept {
    // Phase 6b: DecodeFrameAsync(NULL bitstream) 排空内部 surface 后 MFXVideoDECODE_Close。
}

void CodecOneVPL::stop() noexcept {
    if (!impl_) return;
    // Phase 6b: MFXVideoDECODE_Close + MFXClose + (vpl) MFXUnload。
    impl_->unload_sdk();
}

CodecOneVPL::StartReason CodecOneVPL::last_start_reason() const noexcept {
    return impl_ ? impl_->last_reason : StartReason::device_invalid;
}

bool CodecOneVPL::probe_sdk_dll_present() noexcept {
    HMODULE h = ::LoadLibraryW(L"vpl.dll");
    if (h) { ::FreeLibrary(h); return true; }
    h = ::LoadLibraryW(L"libmfx.dll");
    if (h) { ::FreeLibrary(h); return true; }
    return false;
}

const char* CodecOneVPL::reason_label(StartReason r) noexcept {
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
