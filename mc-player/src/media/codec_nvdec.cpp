#include "media/codec_nvdec.h"

#include <dxgi.h>

#include "media/codec_vendor_base.h"
#include "pal/error.h"
#include "pal/log.h"

using Microsoft::WRL::ComPtr;

namespace mcp::media {

namespace {

// nvcuvid.dll 关键入口 13 个 — 对位 NVIDIA Video Codec SDK 公共接口。
// typedef 按 NVIDIA 公开文档自描述,不引用 SDK 头;Phase 5b 实装解码循环时使用。
//
//   cuvidCreateVideoParser / cuvidParseVideoData / cuvidDestroyVideoParser
//   cuvidCreateDecoder / cuvidDecodePicture / cuvidMapVideoFrame /
//   cuvidUnmapVideoFrame / cuvidDestroyDecoder
//   cuCtxCreate_v2 / cuCtxDestroy_v2 / cuCtxPushCurrent_v2 / cuCtxPopCurrent_v2
//   cuD3D11CtxCreate_v2
//
// 仅记录"是否全部能 GetProcAddress"作为 sdk_init_failed 判据;实际签名 typedef
// 留给 Phase 5b。
constexpr const char* kEntryPoints[] = {
    "cuvidCreateVideoParser",
    "cuvidParseVideoData",
    "cuvidDestroyVideoParser",
    "cuvidCreateDecoder",
    "cuvidDecodePicture",
    "cuvidMapVideoFrame64",     // 64 后缀:NVIDIA 文档建议 64-bit dev pointer
    "cuvidUnmapVideoFrame64",
    "cuvidDestroyDecoder",
    "cuCtxCreate_v2",
    "cuCtxDestroy_v2",
    "cuCtxPushCurrent_v2",
    "cuCtxPopCurrent_v2",
    "cuD3D11CtxCreate_v2",
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

struct CodecNvdec::Impl {
    Config                       cfg;
    HMODULE                      nvcuvid_handle = nullptr;
    CodecNvdec::StartReason      last_reason    = CodecNvdec::StartReason::ok;

    bool load_sdk() noexcept {
        nvcuvid_handle = ::LoadLibraryW(L"nvcuvid.dll");
        return nvcuvid_handle != nullptr;
    }

    bool probe_entry_points() const noexcept {
        if (!nvcuvid_handle) return false;
        for (const char* name : kEntryPoints) {
            // cu* 入口在某些 driver 版本可能不在 nvcuvid.dll 而在 nvcuda.dll;
            // Phase 5b 时按需扩展双 module 探测。当前简化:cu* 缺失也算 sdk_init_failed
            // (会在 NV machine 上触发,届时按需修订)。
            if (::GetProcAddress(nvcuvid_handle, name) == nullptr) {
                return false;
            }
        }
        return true;
    }

    void unload_sdk() noexcept {
        if (nvcuvid_handle) {
            ::FreeLibrary(nvcuvid_handle);
            nvcuvid_handle = nullptr;
        }
    }
};

CodecNvdec::CodecNvdec(Config cfg) : impl_{std::make_unique<Impl>()} {
    impl_->cfg = std::move(cfg);
}

CodecNvdec::~CodecNvdec() { stop(); }

mc_status_t CodecNvdec::start() noexcept {
    if (!impl_->cfg.device) {
        impl_->last_reason = StartReason::device_invalid;
        return MC_ERR_INVALID_ARG;
    }

    const uint32_t vid = query_vendor_id(impl_->cfg.device.Get());
    if (vid != kVendorIdNvidia) {
        impl_->last_reason = StartReason::vendor_mismatch;
        MCP_LOGF(pal::LogLevel::info,
                 "CodecNvdec: vendor mismatch (adapter VendorId=0x%04X, expected 0x10DE)",
                 vid);
        return MC_ERR_UNSUPPORTED;
    }

    if (!impl_->load_sdk()) {
        impl_->last_reason = StartReason::sdk_missing;
        MCP_LOG_INFO("CodecNvdec: nvcuvid.dll not present (sdk_missing)");
        return MC_ERR_NO_HARDWARE;
    }

    if (!impl_->probe_entry_points()) {
        impl_->unload_sdk();
        impl_->last_reason = StartReason::sdk_init_failed;
        MCP_LOG_WARN("CodecNvdec: nvcuvid.dll loaded but required entry points missing");
        return MC_ERR_INTERNAL;
    }

    // Phase 5b: cuvid parser / decoder / CUDA context 创建 + cuvidParseVideoData
    // 回调 + cuvidMapVideoFrame64 → cudaGraphicsD3D11RegisterResource → D3D11 fence。
    // 当前结构性骨架:卸载 SDK 让 controller 降到档 2 DXVA-direct。
    impl_->unload_sdk();
    impl_->last_reason = StartReason::sdk_decode_pending;
    MCP_LOG_INFO("CodecNvdec: SDK loaded, decode pipeline pending Phase 5b — fall back to tier 2");
    return MC_ERR_UNSUPPORTED;
}

void CodecNvdec::submit(std::vector<uint8_t> /*au_bytes*/, int64_t /*pts_us*/,
                          int64_t /*arrival_qpc_ns*/) noexcept {
    // Phase 5b: cuvidParseVideoData(parser, &pkt) where pkt.flags=CUVID_PKT_ENDOFPICTURE
    //          + per-frame callback dispatches cuvidDecodePicture。
    // 当前 stub:start() 返 MC_ERR_UNSUPPORTED 后此方法不会被 controller 调用。
}

void CodecNvdec::flush() noexcept {
    // Phase 5b: cuvidParseVideoData with CUVID_PKT_ENDOFSTREAM + DPB 回收。
}

void CodecNvdec::stop() noexcept {
    if (!impl_) return;
    // Phase 5b: cuvidDestroyVideoParser / cuvidDestroyDecoder / cuCtxDestroy_v2。
    impl_->unload_sdk();
}

CodecNvdec::StartReason CodecNvdec::last_start_reason() const noexcept {
    return impl_ ? impl_->last_reason : StartReason::device_invalid;
}

bool CodecNvdec::probe_sdk_dll_present() noexcept {
    HMODULE h = ::LoadLibraryW(L"nvcuvid.dll");
    if (!h) return false;
    ::FreeLibrary(h);
    return true;
}

const char* CodecNvdec::reason_label(StartReason r) noexcept {
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
