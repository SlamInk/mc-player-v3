/*
 * DXGI 能力快照 — 启动期一次性枚举，缓存。
 *
 * ADD §7.3.1 / §9.1：枚举 IDXGIAdapter1，对每个 adapter 探测 H.264 / H.265 / AV1 关键 profile
 * 的硬解支持与最大分辨率，缓存后续不再重复探测。
 *
 * 探测与运行解耦：探测只在 caps probe 临时 D3D11 device 上做，运行时 device 单独建。
 */

#ifndef MC_PLAYER_PAL_DXGI_CAPS_PROBE_H_
#define MC_PLAYER_PAL_DXGI_CAPS_PROBE_H_

#include <Windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

#include "mc-player/mc_player_types.h"

namespace mcp::pal {

struct AdapterCaps {
    LUID            luid{};
    std::wstring    description;
    uint64_t        dedicated_video_memory   = 0;
    uint64_t        dedicated_system_memory  = 0;
    uint64_t        shared_system_memory     = 0;
    bool            is_software              = false;

    // 解码能力 — 探测以 D3D11VA `CheckVideoDecoderFormat` 为准。
    bool            h264_supported           = false;
    bool            h264_4k_supported        = false;
    bool            hevc_main_supported      = false;
    bool            hevc_main10_supported    = false;
    bool            av1_supported            = false;

    // dual-bind（BIND_DECODER | BIND_SHADER_RESOURCE）支持。
    bool            dual_bind_supported      = false;

    // ALLOW_TEARING（应用层 VRR 兼容呈现的权威 API）。
    bool            allow_tearing_supported  = false;

    // outputs：每个 monitor handle 对应的 adapter，用于 HWND→adapter 解析。
    std::vector<HMONITOR> monitors;

    // Vendor SDK 直驱档 1 探测（ADR-015 / plan Phase 5/6/7）。
    // 仅记录 SDK DLL 是否在搜索路径,实际是否能 decode 由具体 codec 实装 start() 探测。
    uint32_t vendor_id              = 0;       // DXGI_ADAPTER_DESC1::VendorId
    bool     nvcuvid_dll_present    = false;   // nvcuvid.dll (NVIDIA NVDEC)
    bool     onevpl_dll_present     = false;   // vpl.dll 或 libmfx.dll (Intel oneVPL / Media SDK)
    bool     amf_dll_present        = false;   // amfrt64.dll (AMD AMF)
};

class DxgiCapsProbe {
public:
    DxgiCapsProbe()                                = default;
    DxgiCapsProbe(const DxgiCapsProbe&)            = delete;
    DxgiCapsProbe& operator=(const DxgiCapsProbe&) = delete;

    /// 枚举所有 adapter 并填充快照。可重复调用以刷新（e.g. WM_DISPLAYCHANGE）。
    mc_status_t probe() noexcept;

    [[nodiscard]] const std::vector<AdapterCaps>& adapters() const noexcept { return adapters_; }

    /// HWND → 优先 adapter 解析。
    [[nodiscard]] const AdapterCaps* find_by_hwnd(HWND hwnd) const noexcept;

    /// LUID 显式覆盖路径。
    [[nodiscard]] const AdapterCaps* find_by_luid(LUID luid) const noexcept;

    /// 取 factory（device-lost 全恢复时必须重建——caller 自己 reset，本类不缓存调用方 factory）。
    [[nodiscard]] Microsoft::WRL::ComPtr<IDXGIFactory6> factory() const noexcept { return factory_; }

private:
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory_;
    std::vector<AdapterCaps>              adapters_;
};

}  // namespace mcp::pal

#endif  // MC_PLAYER_PAL_DXGI_CAPS_PROBE_H_
